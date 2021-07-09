#!/bin/bash

bin/server test/config/test2_config.txt &
SERVER_PID=$!

# write 'test/sample_files/big_file1' and 'test/sample_files/big_file2'
bin/client -p -f tmp/filestorageserver.sk -W test/sample_files/big_file1,test/sample_files/big_file2

# write 'test/sample_files/big_file3' which will cause the first file written to be removed,
# which will be saved in 'tmp/removed_files/'
mkdir -p tmp/removed_files/
bin/client -p -f tmp/filestorageserver.sk -W test/sample_files/big_file3 -D tmp/removed_files/

kill -s SIGHUP $SERVER_PID
wait $SERVER_PID
