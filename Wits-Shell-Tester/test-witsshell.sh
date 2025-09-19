#! /bin/bash

if ! [[ -x witsshell ]]; then
    echo "wish executable does not exist"
    exit 1
fi

tester/run-tests.sh -c 


