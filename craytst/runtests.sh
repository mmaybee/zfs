#!/usr/bin/bash
#
# Script invoked by nightly ZFS checker if changes were found in the source tree
# the version being tested is passed as -r <version>
# the branch being tested is passed as -b <branch>
# We run both the zfs-tests.sh functional test script and then up to 8 hours
# of the zfs stress test zloop.sh
#
set -x
TESTED_VERSION_FILE=tested_version
LOCKFILE=zfstest_running.lck
CRONTAB=crontab.new
while getopts b:r: option
do
case "${option}"
in
r) TEST_VERSION=$OPTARG;;
b) BRANCH=$OPTARG;;
esac
done
echo "===> Test execution script starting <==="
#
# Remove the run tests on reboot from crontab, this is the requested test
#
echo "SHELL=/usr/bin/bash" > ${CRONTAB}
echo "# Run the checker every night at 10pm" >> ${CRONTAB}
echo "0 22 * * *      $HOME/autotest.sh -b ${BRANCH}" >> ${CRONTAB}
echo "# Following line only if changes in ZFS have been detected" >> ${CRONTAB}
echo "# Will cause the run tests script to be invoked after a reboot" >> ${CRONTAB}
echo "# @reboot         $HOME/runtests.sh -l <logfile>" >> ${CRONTAB}
crontab ${CRONTAB}
if [[ -z ${TEST_VERSION+x} ]]; then
	echo "ERROR: Test source rev not specified - exiting"
	rm -f ${LOCKFILE}
	exit 1
fi
if [[ ! -e ${LOCKFILE} ]]; then
echo "ERROR: No lock file detected - exiting"
	exit 1
fi

DISKS=$(< test_disks)
export DISKS
#
# Clean up things that may cause tests to fail.
#
pkill mkbusy
cd /zfstests
sudo rm -f /etc/hostid || true
sudo zpool import -f testpool1 || true
sudo zpool destroy -f testpool1 || true
sudo zpool import -f testpool || true
sudo zpool destroy -f testpool || true
echo "===> starting Functional tests at `date` <==="
/usr/share/zfs/zfs-tests.sh -v -x -r ${HOME}/skiptests.run
echo "===> Functional test ended at `date` <==="
#
# Preserve previous stress run cores, older cores are discarded.
#
sudo rm -rf ${BRANCH}/coredir.save
sudo mv ${BRANCH}/coredir ${BRANCH}/coredir.save
sudo mkdir -p ${BRANCH}/coredir
sudo rm -rf ${BRANCH}/basedir
sudo mkdir -p ${BRANCH}/basedir || true
# Run the stress test for up to 8 hours or till 8 cores drop.
let RUNTIME=8*60*60
NCORES=8
echo "===> Stress test starting <==="
sudo /usr/share/zfs/zloop.sh -t ${RUNTIME} -m ${NCORES} -c ./${BRANCH}/coredir -f ./${BRANCH}/basedir
echo "===> Stress test ended at `date` <==="
echo "ZFS tests completed on branch ${BRANCH}" | mail -s "ZFS Nightly test finished" zfstestnotify
#
# Clean out log files older than 1 month
#
echo ${TEST_VERSION} > /zfstests/${BRANCH}/${TESTED_VERSION_FILE}
rm -f ${HOME}/${LOCKFILE}
find /zfstests/${BRANCH}/logs -mtime +30 -exec rm -f {} \;
exit 0
