#!/bin/bash
set -x 
USERID=$(stat -c %u .)
GROUPID=$(stat -c %g .)
HOME=$(cd ..;pwd)
export PANDOC=false
groupadd -g $GROUPID rpmgrp
useradd -u $USERID -g $GROUPID -M rpmuser
SUDO () {
    perl -e 'use POSIX qw(setuid setgid); setgid('${GROUPID}');setuid('${USERID}'); exec @ARGV' -- "$@"
}
SUDO rpmbuild -v -ba ./SOURCES/dovecot.spec
