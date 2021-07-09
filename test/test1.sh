#!/bin/bash

# without --error-exit-code=1, valgrind would always return the value of the process being simulated
valgrind --leak-check=full --error-exitcode=1 bin/server test/config/test1_config.txt &
SERVER_PID=$!

# write 'test/sample_files/file1' and 'test/sample_files/file2',
# then read and store them in 'tmp/read_files/'
mkdir -p tmp/read_files/
bin/client -p -t 200 -f tmp/filestorageserver.sk -W test/sample_files/file1,test/sample_files/file2 -r test/sample_files/file1,test/sample_files/file2 -d tmp/read_files/

# recursively visit and write files in 'test/sample_files/sample_dir/',
# then read all files from the server (make a snapshot) and store them in 'tmp/snapshot/'
mkdir -p tmp/snapshot/
bin/client -p -t 200 -f tmp/filestorageserver.sk -w test/sample_files/sample_dir,0 -R 0 -d tmp/snapshot/

# lock a file and then delete it
bin/client -p -t 200 -f tmp/filestorageserver.sk -l test/sample_files/file1 -c test/sample_files/file1

# lock a file and unlock it right after (3 seconds), another client tries to lock it but has to wait
bin/client -p -t 3000 -f tmp/filestorageserver.sk -l test/sample_files/file2 -u test/sample_files/file2 &
bin/client -p -t 0 -f tmp/filestorageserver.sk -l test/sample_files/file2

# print help
bin/client -h

kill -s SIGHUP $SERVER_PID
wait $SERVER_PID
