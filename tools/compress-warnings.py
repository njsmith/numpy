#!/usr/bin/env python

import sys
import os
from collections import defaultdict
import re

if sys.version_info[0] < 3:
    # unbuffered
    infile = os.fdopen(0, "r", 0)
    outfile = os.fdopen(1, "w", 0)
else:
    # line buffered
    infile = os.fdopen(0, "r", buffering=1)
    outfile = os.fdopen(1, "w", buffering=1)

number_re = re.compile("-?[0-9]+")
def generify(message):
    yield message
    yield number_re.sub("NNN", message)

maybe_clusters = defaultdict(set)

warning_counts = defaultdict(int)

# Warnings look like:
# /home/travis/.../sklearn/linear_model/randomized_l1.py:53: DeprecationWarning: This function is deprecated. Please call randint(0, 1 + 1) instead
#   for _ in range(n_resampling)):
warn_re = re.compile("(?P<place>.*:[0-9]+): "
                     "(?P<message>.*Warning: .*)")

while True:
    line = infile.readline()
    if not line:
        break
    match = warn_re.match(line)
    if match:
        message = match.group("message")
        # Warnings are always followed by a line showing the code that
        # triggered the warning, which we don't care about.
        # Sometimes there are extra lines if the warning message itself is
        # multiple lines long. We can sometimes detect this because the code
        # display is always indented, so while we get non-indented lines we
        # can be sure we haven't seen the code display.
        while True:
            follower = infile.readline()
            if not follower.startswith("  "):
                # still getting message... append and look for more
                message += follower
            else:
                # follower is code; discard and move on
                break
        message = message.strip().replace("\r", " ").replace("\n", " ")
        warning_counts[message] += 1
        #warning_places[message].add(match.group("place"))
        for generic in generify(message):
            maybe_clusters[generic].add(message)
    else:
        outfile.write(line)

if warning_counts:
    # Collapse clusters: big clusters "win" and lay claim to their contents
    all_warnings = set(warning_counts)
    cluster_assignments = {}
    clusters = sorted(maybe_clusters.items(),
                      key=lambda item: len(item[1]),
                      reverse=True)
    for g_message, cluster in clusters:
        # Don't try to generify messages that are in a cluster by themself
        if len(cluster) == 1:
            g_message = list(cluster)[0]
        for message in cluster:
            cluster_assignments.setdefault(message, g_message)
    #import pdb; pdb.set_trace()
    cluster_counts = defaultdict(int)
    cluster_variants = defaultdict(int)
    for message, g_message in cluster_assignments.items():
        cluster_counts[g_message] += warning_counts[message]
        cluster_variants[g_message] += 1

    outfile.write("\n")
    outfile.write("Warning summary:\n")
    max_count = float(max(cluster_counts.values()))
    for g_message in sorted(cluster_counts):
        c = cluster_counts[g_message]
        outfile.write("\n  %s\n" % (g_message,))
        outfile.write("    Seen %s times (%s variants)\n"
                     % (c, cluster_variants[g_message]))
        outfile.write("  " + "*" * int(round(c / max_count * 78)) + "\n")

outfile.close()
