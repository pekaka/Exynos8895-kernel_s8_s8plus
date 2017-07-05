#!/system/bin/sh

log_print() {
  echo "$1" >> /dev/.kinit.log
}

if [ -f "/dev/.kinit.log" ]; then
  log_print "next start"
  exit;
fi

log_print "start"

/sbin/resetprop -v -n ro.boot.warranty_bit 0
/sbin/resetprop -v -n ro.warranty_bit 0

# Synapse permissions
chmod 666 /sys/module/workqueue/parameters/power_efficient

# Mount root as RW to apply tweaks and settings
ROOTFS_RO=$(busybox mount | grep 'rootfs' | grep -c 'ro,')
if [ "$ROOTFS_RO" -eq "1" ]; then
    busybox mount -o remount,rw /
fi
if [ "$(busybox mount | grep 'rootfs' | grep -c 'ro,')" -eq "1" ]; then
    su -c "busybox mount -o remount,rw /"
fi
if [ "$(busybox mount | grep 'rootfs' | grep -c 'ro,')" -eq "1" ]; then
    log_print "can't mount rootfs"
else
    chmod -R 755 /res/*
    ln -fs /res/synapse/uci /sbin/uci
    /sbin/uci

    # Mount root as RO
    if [ "$ROOTFS_RO" -eq "1" ]; then
        busybox mount -o remount,ro /
        if [ "$(busybox mount | grep 'rootfs' | grep -c 'rw,')" -eq "1" ]; then
            su -c "busybox mount -o remount,ro /"
        fi
    fi
fi

# default kernel params
/sbin/kernel_params.sh

# Make internal storage directory.
if [ ! -d /data/pekaka ]; then
    DATA_RO=$(busybox mount | grep ' /data ' | grep -c 'ro,')
    if [ "$DATA_RO" -eq "1" ]; then
        busybox mount -o remount,rw /data
    fi
    mkdir /data/pekaka
    if [ "$DATA_RO" -eq "1" ]; then
        busybox mount -o remount,ro /data
    fi
fi;

# init.d support
if [ ! -e /system/etc/init.d ]; then
    SYSTEM_RO=$(busybox mount | grep ' /system ' | grep -c 'ro,')
    if [ "$SYSTEM_RO" -eq "1" ]; then
        busybox mount -o remount,rw /system
    fi
    mkdir /system/etc/init.d
    chown -R root.root /system/etc/init.d
    chmod -R 755 /system/etc/init.d
    if [ "$SYSTEM_RO" -eq "1" ]; then
        busybox mount -o remount,ro /system
    fi
fi

# start init.d
for FILE in /system/etc/init.d/*; do

    su -c ${FILE}
    resSU=$?
    if [ $resSU == 0 ]; then
        log_print "SU run ${FILE}";
    else
        /system/bin/sh ${FILE}
        log_print "SH run ${FILE}";
    fi;

done;

