#!/bin/bash
# Swap the CX2388x card from cxadc (raw ADC) to cx8800 (the chip's own PAL
# decoder), capture decoded video, and swap back with cxadc's parameters
# restored. Usage: cx8800_ab.sh to-v4l2 | grab <name> <seconds> | to-cxadc
set -euo pipefail
PARAMS=/var/tmp/palindrome/cxadc_params.saved
OUT=/var/tmp/palindrome/captures
case "${1:?to-v4l2|grab|to-cxadc}" in
  to-v4l2)
    # snapshot cxadc's runtime parameters (module reload would reset them)
    for f in /sys/class/cxadc/cxadc0/device/parameters/*; do
      echo "$(basename $f)=$(cat $f)"
    done > $PARAMS
    sudo rmmod cxadc
    sudo modprobe cx8800
    for i in $(seq 50); do [ -e /dev/video0 ] && break; sleep 0.2; done
    v4l2-ctl -d /dev/video0 --list-inputs
    # composite input; PAL
    v4l2-ctl -d /dev/video0 --set-input 1 || v4l2-ctl -d /dev/video0 --set-input 0
    v4l2-ctl -d /dev/video0 --set-standard pal
    v4l2-ctl -d /dev/video0 --get-fmt-video
    ;;
  grab)
    NAME=${2:?name} SECS=${3:-5}
    ffmpeg -hide_banner -loglevel warning -y -f v4l2 -standard PAL \
      -i /dev/video0 -t "$SECS" -c:v rawvideo -pix_fmt yuv420p \
      "$OUT/${NAME}_cx8800.y4m"
    ls -la "$OUT/${NAME}_cx8800.y4m"
    ;;
  to-cxadc)
    sudo rmmod cx8800 cx88xx 2>/dev/null || sudo rmmod cx8800 || true
    sudo rmmod cx8802 2>/dev/null || true
    sudo modprobe cxadc
    for i in $(seq 50); do [ -e /dev/cxadc0 ] && break; sleep 0.2; done
    # restore the saved parameters
    while IFS== read -r k v; do
      echo "$v" | sudo tee /sys/class/cxadc/cxadc0/device/parameters/$k >/dev/null || echo "note: $k not restored"
    done < $PARAMS
    for f in /sys/class/cxadc/cxadc0/device/parameters/*; do echo "$(basename $f)=$(cat $f)"; done
    ;;
esac
