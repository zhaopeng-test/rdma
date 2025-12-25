#!/bin/bash

gcc server.c -o server -libverbs

gcc client.c -o client -libverbs