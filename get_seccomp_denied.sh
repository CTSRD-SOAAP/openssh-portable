#!/bin/sh
grep "SC_DENY" sandbox-seccomp-filter.c | grep -v '#define' | perl -pe 's/.*SC_DENY\((.+)\).*/\1/' > seccomp_denied.txt
