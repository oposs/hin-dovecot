SSL fingerprint extension
=========================
  (by Oetiker+Partner AG, Olten, www.oetiker.ch)

  Fritz Zaucker <fritz.zaucker@oetiker.ch>   2020-01-17

History
-------

These patches were ported to dovecot-2.2.x and then 2.3.x from a patch serie
done on Dovecot 2.0.16 in 2011/2012

Features
--------

Added Certificate and Certificate Checks to Dovecot 2.3.x

- ssl_verify_depth:      will check the maximal certificate chain depth

- ssl_cert_md_algorithm: will check the corresponding certificate
                         fingerprint algorithm (md5/sha1/...)

- cert_loginname:        will handle the loginname included in special
                         client certificates (x509 fields)

- cert_fingerprint:      allows to access the fingerprint of a certificate
                         inbound of the dovecot (used for select and compare
                         with LDAP backend where the fingerprint of a user
                         is stored)

Creating a new version
----------------------

git checkout hin-patch-2.3.16
git fetch upstream
git checkout -b hin-patch-NEW_VERSION
git rebase upstream/release-NEW_VERSION

Fix whatever is needed or add new features.

Notes for setup and testing:
----------------------------

- Create RPMs by running

  cd rpmbuild
  ./BUILD.sh

- Either install your CA certificates with matching CRL files
  (see https://wiki.dovecot.org/SSL/DovecotConfiguration#Client_certificate_verification.2Fauthentication)
  or add    

      ssl_require_crl = no

  in your dovecot.conf file (default is true).

  Otherwise your client certificates will not be considered valid.

- For testing (assuming configured for IMAP) you can use

  openssl s_client -servername yourhost.yourdomain -connect yourserver:443 -cert client-certificate.pem

  and then enter

      a001 login user secret

  (see https://gist.github.com/mtigas/952344 for certificate creation
   and https://en.wikipedia.org/wiki/Internet_Message_Access_Protocol#Dialog_example for IMAP).

  and watching your dovecot log file.

- Expired client certificates are detected and treated as invalid.

  This will prevent the extraction of peer_name and fingerprint from the certificate.
  This SHOULD prevent authentication (e.g. towards IMAP), but this has not yet been verified).

  Use  
      ssl_cert_info  = yes
      ssl_cert_debug = yes

  to verify peername and fingerprint of your client certificates.
