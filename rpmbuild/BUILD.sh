#! /bin/bash

###################################################
###
### build RPMs using the official SRPM's .spec file
### from the current patched branch
###
###################################################

CORE_VERSION=2.3.16
SUB_VERSION=2
SRC_VERSION=${CORE_VERSION}-${SUB_VERSION}
TAR_VERSION=${CORE_VERSION}.${SUB_VERSION}
OP_RELEASE=-op1
VERSION=${CORE_VERSION}${OP_RELEASE}

# make sure to be on the right branch
# git checkout hin-patch-$SRC_VERSION
# git pull

# create clean source tar ball
pushd ../ > /dev/null
#make distclean
rm -f dovecot-$CORE_VERSION
ln -s ./ dovecot-$CORE_VERSION
tar zcf dovecot_$CORE_VERSION.orig.tar.gz --exclude dovecot_$CORE_VERSION.orig.tar.gz dovecot-$CORE_VERSION/*
rm -f dovecot-$CORE_VERSION
popd > /dev/null

# get SRPM and extract .spec file

# you might have to fix the dovecot.spec file and the patch
# if the build below fails
rm -f dovecot-${SRC_VERSION}.src.rpm
wget https://repo.dovecot.org/ce-${CORE_VERSION}/centos/6/SRPMS/${SRC_VERSION}_ce/dovecot-${SRC_VERSION}.src.rpm
mkdir -p SOURCES && cd SOURCES
rpm2cpio ../dovecot-${SRC_VERSION}.src.rpm | cpio -idmv
cp dovecot.spec dovecot.spec.orig
patch -p0 < ../dovecot.spec-hin.patch

# the SRC RPM might not have the correct source tarball
# move our own from above here
rm -f dovecot_${CORE_VERSION}.orig.tar.gz
mv ../../dovecot_$CORE_VERSION.orig.tar.gz ./

docker-compose build
docker-compose run rpmbuild
cd ..
rm -rf dovecot-${VERSION}.src.rpm* SOURCES
ls
echo "RPMs are in RPMS/ SRPMS/"
