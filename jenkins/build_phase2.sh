#
# Build RPMs for crayzfs
#
#	The RPM files will be named as follows:
#
#	<pn>-<uv>-<rn>_<#c>_<ch>.el7.centos.x86_64.rpm
#
#	Where:	pn	= package name (e.g. zfs)
#		uv	= upstream ZFS version base (e.g. 0.8.0)
#		rn	= cray release base (e.g. x3.2)
#		#c	= number of commits in this branch
#		ch	= hash ID for latest commit
#
REPO=${JP_REPO:-zfs}
PROJECT=${JP_NEO_RELEASE:-NEO3.X}
SCM_URL=${JP_SCM_URL:-http://es-gerrit.dev.cray.com/zfs}
VERS_BASE=${JP_VERS_BASE:-x3.2}
BUILD_NUMBER=${BUILD_NUMBER:-1}
WORKSPACE=${WORKSPACE:-$(pwd)}

RELEASE_ID=$VERS_BASE

mv META META.default
echo "Meta: 1" > META
echo "Name: $REPO" >> META
echo "Version: 0.8.0" >> META
echo "Release: $RELEASE_ID" >> META
echo "License: CDDL" >> META
echo "Author: Cray" >> META

MOCK="/usr/bin/mock -r mock_${PROJECT}"

# Initialize the chroot build environment for this configuration
${MOCK} --init
if [ "$?" != 0 ] ; then
	echo "BUILD FAILED"
	exit -1
fi

# Install needed packages not already provided by the config
${MOCK} --install autoconf automake libtool zlib-devel libuuid-devel libblkid-devel openssl-devel kernel-devel rpm-build systemd-devel libattr-devel libaio-devel libffi-devel git
if [ "$?" != 0 ] ; then
	echo "BUILD FAILED"
	exit -1
fi

# Copy in the source
rm -rf RPMBUILD
${MOCK} --copyin ${WORKSPACE} /build/zfs
if [ "$?" != 0 ] ; then
	echo "BUILD FAILED"
	exit -1
fi

# Build the rpms in the chroot environment
cat << EOF | ${MOCK} shell
cd /build/zfs

# Autogen
sh ./autogen.sh
rval=\$?
if [ "\$rval" != "0" ] ; then
	exit \$rval
fi

# Configure
./configure --with-spec=redhat
rval=\$?
if [ "\$rval" != "0" ] ; then
	exit \$rval
fi

# Make
make rpms
rval=\$?

mkdir RPMBUILD
mv *.rpm RPMBUILD

exit \$rval
EOF
if [ "$?" != 0 ] ; then
	echo "BUILD FAILED"
	exit -1
fi

mv META META.jenkins
mv META.default META

# Copy out the rpms
${MOCK} --copyout /build/zfs/RPMBUILD RPMBUILD
if [ "$?" != 0 ] ; then
	echo "BUILD FAILED"
	exit -1
fi

# Clean up
${MOCK} --clean

echo "Complete Build"
exit 0
