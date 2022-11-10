#!/bin/bash
for file in $(find ./* -name "*" -type f); do
  vim -S replace_dictory -c wq $file
done
