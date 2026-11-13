#!/bin/sh
# Build custom_surface and run it on the head unit over SSH.
#
#   ./cmu.sh build              cross-compile out/custom_surface (Docker)
#   ./cmu.sh deploy             build, then copy to the head unit (/tmp/custom_surface, gone after reboot)
#   ./cmu.sh start [options]    start it; options go to custom_surface, e.g. -d or -t 60
#   ./cmu.sh stop               hide the window, give the keyboard back to the HMI, exit
#
# CAR_HOST — the IP address of YOUR head unit. There is no default, set it yourself:
#   CAR_HOST=192.168.1.50 ./cmu.sh deploy
# CAR_USER — SSH user (default jci).
set -e
cd "$(dirname "$0")"

DIR=/tmp/custom_surface
LOG=$DIR/log.txt
cmd=${1:-help}
[ $# -gt 0 ] && shift

# Run a shell command on the head unit. One SSH connection is shared by consecutive
# calls, so the password is asked once.
car() {
    : "${CAR_HOST:?is not set. Use the IP address of your head unit: CAR_HOST=192.168.1.50 $0 $cmd}"
    ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new \
        -o ControlMaster=auto -o ControlPath=/tmp/cmu-ssh-%C -o ControlPersist=30 \
        "${CAR_USER:-jci}@$CAR_HOST" "$1"
}

case $cmd in
build)
    docker build --output out . ;;
deploy)
    "$0" build
    car "if pidof custom_surface >/dev/null; then echo 'custom_surface is running, stop it first: $0 stop'; exit 1; fi
         mkdir -p $DIR && cat > $DIR/custom_surface && chmod +x $DIR/custom_surface" < out/custom_surface
    echo "deployed to $CAR_HOST:$DIR/custom_surface" ;;
start)
    car "cd $DIR || exit 1
         if pidof custom_surface >/dev/null; then echo 'already running'; exit 1; fi
         nohup ./custom_surface $* > $LOG 2>&1 < /dev/null &
         sleep 2; cat $LOG" ;;
stop)
    car "pidof custom_surface >/dev/null || { echo 'not running'; exit 0; }
         kill \$(pidof custom_surface)
         for i in 1 2 3 4 5; do sleep 1; pidof custom_surface >/dev/null || break; done
         tail -n 3 $LOG
         if pidof custom_surface >/dev/null; then echo 'still running, see Recovery in README.md'; exit 1; fi" ;;
*)
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
