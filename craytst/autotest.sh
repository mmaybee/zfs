#!/usr/bin/bash
#
# This script will run the zfs functional test script (zfstest) and
# the zfs stress test (zloop) if any change is detected in the zfs
# source tree for the branch specified.
# Test requirements:
#	4 disk drives.  One mounted with a filesystem on /zfstests
#	and 3 scratch disks that are specified as DISKS in the
#	runtests.sh script.  e.g. DISKS='sdd sde sdf'
# The script should be invoked nightly at 10PM via a cron job.
# The -b option specifies the branch to test.
# The -i option specifies to initialize autotest to run nightly on the 
# specified branch.
# The -d option provides the list of scratch disks to use
# If we are invoked with a -f option then the test suites will be run even
# if no source code changes are detected.
#
set -x
DISKS=""
FORCE=no
INIT=no
LOCKFILE=zfstest_running.lck
CRONTAB=crontab.new
LAST_SUCCESSFUL_TEST=0
TESTED_VERSION_FILE=tested_version
while getopts "b:d:fi" option; do
	case ${option} in
		b) BRANCH=$OPTARG;;
		d) DISKS=$OPTARG;;
		f) FORCE=yes;;
		i) INIT=yes;;
		\?) echo usage: "autotest.sh -b <branch> -d \"<disk1> <disk2> <disk3>\" -f -i"
			exit 1
			;;
	esac
done
if [[ $INIT == "yes" ]]; then
	NDISKS=$(echo ${DISKS} | wc -w)
	# Check that we were given a list of 3 scratch disks to use
	if [[ -z ${DISKS+x} || ${NDISKS} != "3" ]]; then
		echo "must specify a list of 3 scratch disks"
		exit 1
	fi
	# Were we given a git branch to test?
	if [[ -z ${BRANCH+x} ]]; then
		echo "Must specify branch to test"
		exit 1
	fi
	MOUNT=$(mount | grep zfstests)
	if [[ ! -d /zfstests || -z ${MOUNT+x} ]]; then
		echo "no /zfstests directory/disk found"
		exit 1
	fi
	# save list of test disks for runtests script to use
	echo ${DISKS} > test_disks
	# Set up current zfs source and checkout the desired branch
	if [[ ! -d zfs ]]; then
		git clone http://es-gerrit.dev.cray.com/zfs.git
	fi
	cd zfs
	git checkout ${BRANCH}
	# Ensure log directory exists
	if [[ ! -d /zfstests/${BRANCH}/logs} ]]; then
		mkdir -p /zfstests/${BRANCH}/logs
	fi
	echo ${LAST_SUCCESSFUL_TEST} > /zfstests/${BRANCH}/tested_version
	rm -f ../${LOCKFILE}
	rm -rf RPMBUILD
	#
	# Set up directories for stress tests
	#
	sudo rm -rf /zfstests/${BRANCH}/coredir
	sudo rm -rf /zfstests/${BRANCH}/basedir
	sudo mkdir /zfstests/${BRANCH}/coredir
	sudo mkdir /zfstests/${BRANCH}/basedir
	# Initialize the cron job to do the nightly check for ZFS changes
	echo "SHELL=/usr/bin/bash" > ${CRONTAB}
	echo "# Run the checker every night at 10pm" >> ${CRONTAB}
	echo "0 22 * * *      $HOME/autotest.sh -b ${BRANCH}" >> ${CRONTAB}
	echo "# Following line only if changes in ZFS have been detected" >> ${CRONTAB}
	echo "# Will cause the run tests script to be invoked after a reboot" >> ${CRONTAB}
	echo "# @reboot  $HOME/runtests.sh -r <version> -b <branch> >> /zfstests/logs/<branch>/<logfile> 2>&1" >> ${CRONTAB}
	crontab ${CRONTAB}
	echo "ZFS branch ${BRANCH} will be checked nightly at 10pm for changes"
	exit 0
fi
NOW=$(date "+%Y-%m-%d_%H.%M.%S")
LOGFILE=nightly_${NOW}.log
# Everything from here on is logged to ${LOGFILE}
{
if [[ -e /zfstests/${BRANCH}/${TESTED_VERSION_FILE} ]]; then
	LAST_SUCCESSFUL_TEST=$(</zfstests/${BRANCH}/${TESTED_VERSION_FILE})
fi
echo "===> Nightly ZFS test run started at ${NOW} <==="
#
# Check/create a test running file to prevent multiple simultaneous 
# runs of the test suites.  If file is more than 2 days old then delete
# it and proceed anyway.
#
if [[ $(find "${LOCKFILE}" -mtime +1 -print) ]]; then
	echo "===> File ${LOCKFILE} exists and is older than 2 days - removing <==="
	rm -f ${LOCKFILE}
fi
if [[ -e ${LOCKFILE} ]]; then
	echo "===> File ${LOCKFILE} exists, tests already running - exiting <==="
	exit 1
fi
echo ${NOW} > ${LOCKFILE}
#
# Pull the latest Gerrit ZFS source tree and see if it has any changes.
#
if [[ ! -d zfs ]]; then
	echo "===> no zfs source tree found - exiting <==="
	rm -f ${LOCKFILE}
fi
cd zfs
git checkout ${BRANCH}
ZFS_CURRENT_REV=$(git rev-list --max-count=1 HEAD | cut -c1-8)
echo "===> Current rev is: ${ZFS_CURRENT_REV} <==="
git pull
ZFS_NEWEST_REV=$(git rev-list --max-count=1 HEAD | cut -c1-8)
echo "===> Newest rev is: ${ZFS_NEWEST_REV} <==="
if [[ ${ZFS_CURRENT_REV} == ${ZFS_NEWEST_REV} ]]; then
	echo "===> No changes to ZFS source detected for branch ${BRANCH} <==="
	if [[ ${FORCE} == "no" && ${LAST_SUCCESSFUL_TEST} == ${ZFS_NEWEST_REV} ]]; then
		echo "===> ZFS rev ${ZFS_NEWEST_REV} has been tested - exiting <==="
		rm -f ../${LOCKFILE}
		exit 0  # Nothing to do
	fi
	#
	# here if test is forced or has not run successfully
	#
	echo "===> testing ZFS rev ${ZFS_NEWEST_REV} <==="
	fi
echo "Running nightly tests on ZFS branch ${BRANCH}, resuts in /zfstests/${BRANCH}/logs/${LOGFILE}" | mail -s "ZFS Nightly test" zfstestnotify
#
# If we are here we need to build and install Latest ZFS and run the
# test scripts.
#
echo "===> Building DEBUG version of latest source on branch ${BRANCH} <==="
#
# Build debug versions of the ZFS RPM's as they do much more error 
# detecting and reporting than the non-debug versions.
# XXX - eventually we want to have the standard Jenkins build generate
# both debug and non-debug RPM versions so we would not need to build them 
#
#
# RPMs are in RPMBUILD
#
#
# Clean away any old RPMs
#
rm -rf RPMBUILD
jenkins/build_phase2.sh DEBUG
#
# Check that we built the requisite number of RPMs to verify the build worked
#
RPMCOUNT=$(ls RPMBUILD | wc -w)
if [[ ! -d RPMBUILD || ${RPMCOUNT} != "16" ]]; then
	echo "ERROR: DEBUG build failed - exiting"
	rm -f ../${LOCKFILE}
	exit 1
fi
cd RPMBUILD
#
# Replace any currently installed ZFS RPM's
#
sudo yum -y remove kmod-zfs kmod-zfs-devel libnvpair1 libuutil1 libzfs2 libzfs2-devel libzpool2 zfs zfs-debuginfo zfs-dkms zfs-dracut zfs-kmod zfs-kmod-debuginfo zfs-test
sudo yum -y install *x86_64.rpm
cd ../..
#
# Create a cron job to run the test suites on reboot and then reboot so we
# have a clean install of the new RPMs and the test suites will eun
# we need the following entries in crontab:
#
#	SHELL=/usr/bin/bash
#	# Run the checker every night at 10pm
#	0 22 * * *	$HOME/autotest.sh -b ${BRANCH}
#	# Following line only if changes in ZFS have been detected
#	# Will cause the run tests script to be invoked after a reboot
#	@reboot		$HOME/runtests.sh -r ${ZFS_NEWEST_REV} -b <branch>  >> /zfstests/${BRANCH}/logs/${LOGFILE} 2>&1
echo "===> Updating crontab to run zfs test suite on next boot <=="
echo "SHELL=/usr/bin/bash" > ${CRONTAB}
echo "# Run the checker every night at 10pm" >> ${CRONTAB}
echo "0 22 * * *      $HOME/autotest.sh -b ${BRANCH}" >> ${CRONTAB}
echo "# Following line only if changes in ZFS have been detected" >> ${CRONTAB}
echo "# Will cause the run tests script to be invoked after a reboot" >> ${CRONTAB}
echo "@reboot         $HOME/runtests.sh -r ${ZFS_NEWEST_REV} -b ${BRANCH} >> /zfstests/${BRANCH}/logs/${LOGFILE} 2>&1" >> ${CRONTAB}
crontab ${CRONTAB}
echo "===> rebooting <==="
sudo sync
sudo reboot -f
} > /zfstests/${BRANCH}/logs/${LOGFILE} 2>&1
