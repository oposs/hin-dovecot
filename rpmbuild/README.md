HOWTO Build RPMS
==========================
Author: Fritz Zaucker
Date:   2020-01-17, Version 2


VERSION=2.3.16
RELEASE=op1

# Create a new hin-patch-$VERSION branch as described in README-hin-patch.md
# (see e.g. in hin-patch-2.3.9 branch)

# Update BUILD.sh script as necessary for new version

# Build RPMS
make
# The generated RPM files are below the RPMS folder

# Upload to HIN download area
make upload

# Cleanup
make clean
