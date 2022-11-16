#!/bin/bash

#vim_command=':1,$s/#include "\(.*\)"/#include <\1>/g'
vim_command=':1,$s/#include "\(.*\)"/#include <\1>/g'

#for file in $(find ./src/* -name "*" -type f); do
for file in $(find ./test/* -name "*" -type f); do
  vim -c "$vim_command" -c wq $file
done

#vim -c "$vim_command" -c wq ./src/codegen.cpp
#echo "$vim_command"
