#!/bin/sh
grep "SC_ALLOW" sandbox-seccomp-filter.c | grep -v '#define' | perl -pe 's/.*SC_ALLOW\((.+)\).*/\1/' > seccomp_allowed.txt
