#!/bin/sh


pactl load-module module-virtual-sink sink_name="vsink0"
pactl load-module module-virtual-sink sink_name="vsink1"
pactl load-module module-virtual-sink sink_name="vsink2"
pactl load-module module-virtual-sink sink_name="vsink3"
pactl load-module module-virtual-sink sink_name="vsink4"
pactl load-module module-virtual-sink sink_name="vsink5"
sleep 1
../sdrpp -r ./root_dev/


