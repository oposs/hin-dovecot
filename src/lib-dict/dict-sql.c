/* Copyright (c) 2005-2008 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "str.h"
#include "sql-api-private.h"
#include "sql-pool.h"
#include "dict-private.h"
#include "dict-sql-settings.h"
#include "dict-sql.h"

#include <unistd.h>
#include <fcntl.h>

#define DICT_SQL_MAX_UNUSED_CONNECTIONS 10

enum sql_recurse_type {
	SQL_DICT_RECURSE_NONE,
	SQL_DICT_RECURSE_ONE,
	SQL_DICT_RECURSE_FULL
};

struct sql_dict {
	struct dict dict;

	pool_t pool;
	struct sql_db *db;
	const char *username;
	const struct dict_sql_settings *set;
	unsigned int prev_map_match_idx;

	unsigned int has_on_duplicate_key:1;
};

struct sql_dict_iterate_context {
	struct dict_iterate_context ctx;
	enum dict_iterate_flags flags;
	char *path;

	struct sql_result *result;
	string_t *key;
	const struct dict_sql_map *map;
	unsigned int key_prefix_len, pattern_prefix_len, next_map_idx;
};

struct sql_dict_transaction_context {
	struct dict_transaction_context ctx;

	struct sql_transaction_context *sql_ctx;

	unsigned int failed:1;
	unsigned int changed:1;
};

static struct sql_pool *dict_sql_pool;

static struct dict *
sql_dict_init(struct dict *driver, const char *uri,
	      enum dict_data_type value_type ATTR_UNUSED,
	      const char *username)
{
	struct sql_dict *dict;
	pool_t pool;

	pool = pool_alloconly_create("sql dict", 1024);
	dict = p_new(pool, struct sql_dict, 1);
	dict->pool = pool;
	dict->dict = *driver;
	dict->username = p_strdup(pool, username);
	dict->set = dict_sql_settings_read(pool, uri);
	if (dict->set == NULL) {
		pool_unref(&pool);
		return NULL;
	}

	/* currently pgsql and sqlite don't support "ON DUPLICATE KEY" */
	dict->has_on_duplicate_key = strcmp(driver->name, "mysql") == 0;

	dict->db = sql_pool_new(dict_sql_pool, driver->name,
				dict->set->connect);
	return &dict->dict;
}

static void sql_dict_deinit(struct dict *_dict)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;

	sql_deinit(&dict->db);
	pool_unref(&dict->pool);
}

static bool
dict_sql_map_match(const struct dict_sql_map *map, const char *path,
		   ARRAY_TYPE(const_string) *values, unsigned int *pat_len_r,
		   unsigned int *path_len_r, bool partial_ok)
{
	const char *path_start = path;
	const char *pat, *field, *p;
	unsigned int len;

	array_clear(values);
	pat = map->pattern;
	while (*pat != '\0' && *path != '\0') {
		if (*pat == '$') {
			/* variable */
			pat++;
			if (*pat == '\0') {
				/* pattern ended with this variable,
				   it'll match the rest of the path */
				len = strlen(path);
				if (partial_ok) {
					/* iterating - the last field never
					   matches fully. if there's a trailing
					   '/', drop it. */
					pat--;
					if (path[len-1] == '/') {
						field = t_strndup(path, len-1);
						array_append(values, &field, 1);
					} else {
						array_append(values, &path, 1);
					}
				} else {
					array_append(values, &path, 1);
					path += len;
				}
				*path_len_r = path - path_start;
				*pat_len_r = pat - map->pattern;
				return TRUE;
			}
			/* pattern matches until the next '/' in path */
			p = strchr(path, '/');
			if (p != NULL) {
				field = t_strdup_until(path, p);
				array_append(values, &field, 1);
				path = p;
			} else {
				/* no '/' anymore, but it'll still match a
				   partial */
				array_append(values, &path, 1);
				path += strlen(path);
				pat++;
			}
		} else if (*pat == *path) {
			pat++;
			path++;
		} else {
			return FALSE;
		}
	}
	if (*pat == '\0')
		return *path == '\0';
	else if (!partial_ok)
		return FALSE;
	else {
		/* partial matches must end with '/' */
		*path_len_r = path - path_start;
		*pat_len_r = pat - map->pattern;
		return pat == map->pattern || pat[-1] == '/';
	}
}

static const struct dict_sql_map *
sql_dict_find_map(struct sql_dict *dict, const char *path,
		  ARRAY_TYPE(const_string) *values)
{
	const struct dict_sql_map *maps;
	unsigned int i, idx, count, len;

	t_array_init(values, dict->set->max_field_count);
	maps = array_get(&dict->set->maps, &count);
	for (i = 0; i < count; i++) {
		/* start matching from the previously successful match */
		idx = (dict->prev_map_match_idx + i) % count;
		if (dict_sql_map_match(&maps[idx], path, values,
				       &len, &len, FALSE)) {
			dict->prev_map_match_idx = idx;
			return &maps[idx];
		}
	}
	return NULL;
}

static void
sql_dict_where_build(struct sql_dict *dict, const struct dict_sql_map *map,
		     const ARRAY_TYPE(const_string) *values_arr,
		     const char *key, enum sql_recurse_type recurse_type,
		     string_t *query)
{
	const char *const *sql_fields, *const *values;
	unsigned int i, count, count2, exact_count;
	bool priv = *key == DICT_PATH_PRIVATE[0];

	sql_fields = array_get(&map->sql_fields, &count);
	values = array_get(values_arr, &count2);
	/* if we came here from iteration code there may be less values */
	i_assert(count2 <= count);

	if (count2 == 0 && !priv) {
		/* we want everything */
		return;
	}

	str_append(query, " WHERE");
	exact_count = count == count2 && recurse_type != SQL_DICT_RECURSE_NONE ?
		count2-1 : count2;
	for (i = 0; i < exact_count; i++) {
		if (i > 0)
			str_append(query, " AND");
		str_printfa(query, " %s = '%s'", sql_fields[i],
			    sql_escape_string(dict->db, values[i]));
	}
	switch (recurse_type) {
	case SQL_DICT_RECURSE_NONE:
		break;
	case SQL_DICT_RECURSE_ONE:
		if (i > 0)
			str_append(query, " AND");
		if (i < count2) {
			str_printfa(query, " %s LIKE '%s/%%' AND "
				    "%s NOT LIKE '%s/%%/%%'",
				    sql_fields[i],
				    sql_escape_string(dict->db, values[i]),
				    sql_fields[i],
				    sql_escape_string(dict->db, values[i]));
		} else {
			str_printfa(query, " %s LIKE '%%' AND "
				    "%s NOT LIKE '%%/%%'",
				    sql_fields[i], sql_fields[i]);
		}
		break;
	case SQL_DICT_RECURSE_FULL:
		if (i < count2) {
			if (i > 0)
				str_append(query, " AND");
			str_printfa(query, " %s LIKE '%s/%%'", sql_fields[i],
				    sql_escape_string(dict->db, values[i]));
		}
		break;
	}
	if (priv) {
		if (count2 > 0)
			str_append(query, " AND");
		str_printfa(query, " %s = '%s'", map->username_field,
			    sql_escape_string(dict->db, dict->username));
	}
}

static int sql_dict_lookup(struct dict *_dict, pool_t pool,
			   const char *key, const char **value_r)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) values;
	struct sql_result *result;
	int ret;

	map = sql_dict_find_map(dict, key, &values);
	if (map == NULL) {
		i_error("sql dict lookup: Invalid/unmapped key: %s", key);
		*value_r = NULL;
		return 0;
	}

	T_BEGIN {
		string_t *query = t_str_new(256);

		str_printfa(query, "SELECT %s FROM %s",
			    map->value_field, map->table);
		sql_dict_where_build(dict, map, &values, key,
				     SQL_DICT_RECURSE_NONE, query);
		result = sql_query_s(dict->db, str_c(query));
	} T_END;

	ret = sql_result_next_row(result);
	if (ret <= 0) {
		if (ret < 0) {
			i_error("dict sql lookup failed: %s",
				sql_result_get_error(result));
		}
		*value_r = NULL;
	} else {
		*value_r =
			p_strdup(pool, sql_result_get_field_value(result, 0));
	}

	sql_result_free(result);
	return ret;
}

static const struct dict_sql_map *
sql_dict_iterate_find_next_map(struct sql_dict_iterate_context *ctx,
			       ARRAY_TYPE(const_string) *values)
{
	struct sql_dict *dict = (struct sql_dict *)ctx->ctx.dict;
	const struct dict_sql_map *maps;
	unsigned int i, count, pat_len, path_len;

	t_array_init(values, dict->set->max_field_count);
	maps = array_get(&dict->set->maps, &count);
	for (i = ctx->next_map_idx; i < count; i++) {
		if (dict_sql_map_match(&maps[i], ctx->path,
				       values, &pat_len, &path_len, TRUE) &&
		    ((ctx->flags & DICT_ITERATE_FLAG_RECURSE) != 0 ||
		     array_count(values)+1 >= array_count(&maps[i].sql_fields))) {
			ctx->key_prefix_len = path_len;
			ctx->pattern_prefix_len = pat_len;
			ctx->next_map_idx = i + 1;
			return &maps[i];
		}
	}
	return NULL;
}

static bool sql_dict_iterate_next_query(struct sql_dict_iterate_context *ctx)
{
	struct sql_dict *dict = (struct sql_dict *)ctx->ctx.dict;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) values;
	const char *const *sql_fields;
	enum sql_recurse_type recurse_type;
	unsigned int i, count;

	map = sql_dict_iterate_find_next_map(ctx, &values);
	if (map == NULL)
		return FALSE;

	T_BEGIN {
		string_t *query = t_str_new(256);

		str_printfa(query, "SELECT %s", map->value_field);
		/* get all missing fields */
		sql_fields = array_get(&map->sql_fields, &count);
		i = array_count(&values);
		if (i == count) {
			/* we always want to know the last field since we're
			   iterating its children */
			i_assert(i > 0);
			i--;
		}
		for (; i < count; i++)
			str_printfa(query, ",%s", sql_fields[i]);
		str_printfa(query, " FROM %s", map->table);

		recurse_type = (ctx->flags & DICT_ITERATE_FLAG_RECURSE) == 0 ?
			SQL_DICT_RECURSE_ONE : SQL_DICT_RECURSE_FULL;
		sql_dict_where_build(dict, map, &values, ctx->path,
				     recurse_type, query);

		if ((ctx->flags & DICT_ITERATE_FLAG_SORT_BY_KEY) != 0) {
			str_append(query, " ORDER BY ");
			for (i = array_count(&values); i < count; i++) {
				str_printfa(query, "%s", sql_fields[i]);
				if (i < count-1)
					str_append_c(query, ',');
			}
		} else if ((ctx->flags & DICT_ITERATE_FLAG_SORT_BY_VALUE) != 0)
			str_printfa(query, " ORDER BY %s", map->value_field);
		ctx->result = sql_query_s(dict->db, str_c(query));
	} T_END;

	ctx->map = map;
	return TRUE;
}

static struct dict_iterate_context *
sql_dict_iterate_init(struct dict *_dict, const char *path, 
		      enum dict_iterate_flags flags)
{
	struct sql_dict_iterate_context *ctx;

	ctx = i_new(struct sql_dict_iterate_context, 1);
	ctx->ctx.dict = _dict;
	ctx->path = i_strdup(path);
	ctx->flags = flags;
	ctx->key = str_new(default_pool, 256);
	str_append(ctx->key, ctx->path);

	if (!sql_dict_iterate_next_query(ctx)) {
		i_error("sql dict iterate: Invalid/unmapped path: %s", path);
		ctx->result = NULL;
		return &ctx->ctx;
	}
	return &ctx->ctx;
}

static int sql_dict_iterate(struct dict_iterate_context *_ctx,
			    const char **key_r, const char **value_r)
{
	struct sql_dict_iterate_context *ctx =
		(struct sql_dict_iterate_context *)_ctx;
	const char *p;
	unsigned int i, count;
	int ret;

	if (ctx->result == NULL)
		return -1;

	while ((ret = sql_result_next_row(ctx->result)) == 0) {
		/* see if there are more results in the next map */
		if (!sql_dict_iterate_next_query(ctx))
			return 0;
	}
	if (ret < 0) {
		i_error("dict sql iterate failed: %s",
			sql_result_get_error(ctx->result));
		return ret;
	}

	/* convert fetched row to dict key */
	str_truncate(ctx->key, ctx->key_prefix_len);
	if (ctx->key_prefix_len > 0 &&
	    str_c(ctx->key)[ctx->key_prefix_len-1] != '/')
		str_append_c(ctx->key, '/');

	count = sql_result_get_fields_count(ctx->result);
	i = 1;
	for (p = ctx->map->pattern + ctx->pattern_prefix_len; *p != '\0'; p++) {
		if (*p != '$')
			str_append_c(ctx->key, *p);
		else {
			i_assert(i < count);
			str_append(ctx->key,
				   sql_result_get_field_value(ctx->result, i));
			i++;
		}
	}

	*key_r = str_c(ctx->key);
	*value_r = sql_result_get_field_value(ctx->result, 0);
	return 1;
}

static void sql_dict_iterate_deinit(struct dict_iterate_context *_ctx)
{
	struct sql_dict_iterate_context *ctx =
		(struct sql_dict_iterate_context *)_ctx;

	if (ctx->result != NULL)
		sql_result_free(ctx->result);
	str_free(&ctx->key);
	i_free(ctx->path);
	i_free(ctx);
}

static struct dict_transaction_context *
sql_dict_transaction_init(struct dict *_dict)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;
	struct sql_dict_transaction_context *ctx;

	ctx = i_new(struct sql_dict_transaction_context, 1);
	ctx->ctx.dict = _dict;
	ctx->sql_ctx = sql_transaction_begin(dict->db);

	return &ctx->ctx;
}

static int sql_dict_transaction_commit(struct dict_transaction_context *_ctx)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	const char *error;
	int ret;

	if (ctx->failed) {
		sql_transaction_rollback(&ctx->sql_ctx);
		ret = -1;
	} else if (_ctx->changed) {
		ret = sql_transaction_commit_s(&ctx->sql_ctx, &error);
		if (ret < 0)
			i_error("sql dict: commit failed: %s", error);
	} else {
		/* nothing to be done */
		ret = 0;
	}
	i_free(ctx);
	return ret;
}

static void sql_dict_transaction_rollback(struct dict_transaction_context *_ctx)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;

	if (_ctx->changed)
		sql_transaction_rollback(&ctx->sql_ctx);
	i_free(ctx);
}

static const char *
sql_dict_set_query(struct sql_dict *dict, const struct dict_sql_map *map,
		   const ARRAY_TYPE(const_string) *values_arr,
		   const char *key, const char *value, bool inc)
{
	const char *const *sql_fields, *const *values;
	unsigned int i, count, count2;
	string_t *prefix, *suffix;

	prefix = t_str_new(64);
	suffix = t_str_new(256);
	str_printfa(prefix, "INSERT INTO %s (%s", map->table, map->value_field);
	str_append(suffix, ") VALUES (");
	if (inc)
		str_append(suffix, value);
	else
		str_printfa(suffix, "'%s'", sql_escape_string(dict->db, value));
	if (*key == DICT_PATH_PRIVATE[0]) {
		str_printfa(prefix, ",%s", map->username_field);
		str_printfa(suffix, ",'%s'",
			    sql_escape_string(dict->db, dict->username));
	}

	/* add the other fields from the key */
	sql_fields = array_get(&map->sql_fields, &count);
	values = array_get(values_arr, &count2);
	i_assert(count == count2);
	for (i = 0; i < count; i++) {
		str_printfa(prefix, ",%s", sql_fields[i]);
		str_printfa(suffix, ",'%s'",
			    sql_escape_string(dict->db, values[i]));
	}

	str_append_str(prefix, suffix);
	str_append_c(prefix, ')');
	if (dict->has_on_duplicate_key) {
		str_printfa(prefix, " ON DUPLICATE KEY UPDATE %s =",
			    map->value_field);
		if (inc)
			str_printfa(prefix, "%s+%s", map->value_field, value);
		else {
			str_printfa(prefix, "'%s'",
				    sql_escape_string(dict->db, value));
		}
	}
	return str_c(prefix);
}

static void sql_dict_set(struct dict_transaction_context *_ctx,
			 const char *key, const char *value)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	struct sql_dict *dict = (struct sql_dict *)_ctx->dict;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) values;

	map = sql_dict_find_map(dict, key, &values);
	if (map == NULL) {
		i_error("sql dict set: Invalid/unmapped key: %s", key);
		ctx->failed = TRUE;
		return;
	}

	T_BEGIN {
		const char *query;

		query = sql_dict_set_query(dict, map, &values, key, value,
					   FALSE);
		sql_update(ctx->sql_ctx, query);
	} T_END;
}

static void sql_dict_unset(struct dict_transaction_context *_ctx,
			   const char *key)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	struct sql_dict *dict = (struct sql_dict *)_ctx->dict;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) values;

	map = sql_dict_find_map(dict, key, &values);
	if (map == NULL) {
		i_error("sql dict unset: Invalid/unmapped key: %s", key);
		ctx->failed = TRUE;
		return;
	}

	T_BEGIN {
		string_t *query = t_str_new(256);

		str_printfa(query, "DELETE FROM %s", map->table);
		sql_dict_where_build(dict, map, &values, key,
				     SQL_DICT_RECURSE_NONE, query);
		sql_update(ctx->sql_ctx, str_c(query));
	} T_END;
}

static void sql_dict_atomic_inc(struct dict_transaction_context *_ctx,
				const char *key, long long diff)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	struct sql_dict *dict = (struct sql_dict *)_ctx->dict;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) values;

	map = sql_dict_find_map(dict, key, &values);
	if (map == NULL) {
		i_error("sql dict atomic inc: Invalid/unmapped key: %s", key);
		ctx->failed = TRUE;
		return;
	}

	T_BEGIN {
		const char *query;

		query = sql_dict_set_query(dict, map, &values, key,
					   dec2str(diff), TRUE);
		sql_update(ctx->sql_ctx, query);
	} T_END;
}

static struct dict sql_dict = {
	MEMBER(name) "sql",

	{
		sql_dict_init,
		sql_dict_deinit,
		sql_dict_lookup,
		sql_dict_iterate_init,
		sql_dict_iterate,
		sql_dict_iterate_deinit,
		sql_dict_transaction_init,
		sql_dict_transaction_commit,
		sql_dict_transaction_rollback,
		sql_dict_set,
		sql_dict_unset,
		sql_dict_atomic_inc
	}
};

static struct dict *dict_sql_drivers;

void dict_sql_register(void)
{
        const struct sql_db *const *drivers;
	unsigned int i, count;

	dict_sql_pool = sql_pool_init(DICT_SQL_MAX_UNUSED_CONNECTIONS);

	/* @UNSAFE */
	drivers = array_get(&sql_drivers, &count);
	dict_sql_drivers = i_new(struct dict, count + 1);

	for (i = 0; i < count; i++) {
		dict_sql_drivers[i] = sql_dict;
		dict_sql_drivers[i].name = drivers[i]->name;

		dict_driver_register(&dict_sql_drivers[i]);
	}
}

void dict_sql_unregister(void)
{
	int i;

	for (i = 0; dict_sql_drivers[i].name != NULL; i++)
		dict_driver_unregister(&dict_sql_drivers[i]);
	i_free(dict_sql_drivers);
	sql_pool_deinit(&dict_sql_pool);
}
