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

RELEASE_ID=$VERS_BASE

mv META META.default
echo "Meta: 1" > META
echo "Name: $REPO" >> META
echo "Version: 0.8.0" >> META
echo "Release: $RELEASE_ID" >> META
echo "License: CDDL" >> META
echo "Author: Cray" >> META

sh ./autogen.sh >& autogen.log
rval=$?
if [ "$rval" != "0" ] ; then
	echo "BUILD FAILED"
	exit $rval
fi

./configure --with-spec=redhat >& configure.log
rval=$?
if [ "$rval" != "0" ] ; then
	echo "BUILD FAILED"
	exit $rval
fi

make rpms >& make.log
rval=$?
if [ "$rval" != "0" ] ; then
	echo "BUILD_FAILED"
	exit $rval
fi

mv META META.jenkins
mv META.default META

echo "Complete Build"
exit 0
