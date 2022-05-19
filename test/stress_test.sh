#!/bin/bash

cmd=$1
tmp_dir="$(pwd)/temp"
port=8002

if [[ $cmd = "install" ]]; then
    # Check if platform has apt
    echo "Installing nginx $cmd" $([[ $cmd -eq "install"  ]])
    exit 1
    if [[ -x "$(command -v apt)" ]]; then
        sudo apt install -y nginx-light
    elif [[ -x "$(command -v yum)" ]]; then
        sudo yum install -y nginx
    else
        echo "No supported package manager found. Please install nginx manually."
        exit 1
    fi
elif [[ $cmd = "init" ]]; then
    # If $tmp_dir does not exist, create it
    if [[ ! -d $tmp_dir ]]; then
        mkdir $tmp_dir
    fi
    # Create three random files of sizes 1Mbyte, 10Mbyte, 100Mbyte
    dd if=/dev/urandom of=$tmp_dir/1M.bin bs=1M count=1
    dd if=/dev/urandom of=$tmp_dir/10M.bin bs=10M count=1
    dd if=/dev/urandom of=$tmp_dir/100M.bin bs=10M count=10
elif [[ $cmd = "start_server" ]]; then
    # Create nginx config file
    cat << EOF > $tmp_dir/nginx.conf
worker_processes  5;  ## Default: 1
error_log  $tmp_dir/error.log;
worker_rlimit_nofile 8192;

events {
  worker_connections  4096;  ## Default: 1024
}
http {
    server {
        listen $port;
        gzip off; # We don't want compression messing with the results
        error_log $tmp_dir/error.log;
        access_log $tmp_dir/access.log;
        autoindex off; # disable directory listing output
        location / {
            root $tmp_dir;
        }
    }
}
EOF
    # Start nginx server
    sudo nginx -c $tmp_dir/nginx.conf
elif [[ $cmd = "stop_server" ]]; then
    # Stop nginx server
    sudo nginx -s stop
elif [[ $cmd = "stress" ]]; then
    # Get number of times to download each file
    times=$2
    ip=$3
    if [[ -z $times ]]; then
        times=1
    fi
    if [[ -z $ip ]]; then
        ip=localhost
    fi
    # Run 100 downloads of all three files and check how many succeeded

    for i in $(seq 1 $times); do
        curl -o /dev/null http://$ip:$port/1M.bin &
        curl -o /dev/null http://$ip:$port/10M.bin &
        curl -o /dev/null http://$ip:$port/100M.bin &
    done
elif [[ $cmd = "clean" ]]; then
    # Remove all files created by init
    rm -rf $tmp_dir
else
    echo "Invalid command $cmd. Please use one of the following commands:"
    echo "install, init, start_server, stop_server, stress, clean"
    exit 1
fi
