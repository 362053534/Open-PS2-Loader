#!/bin/bash
# Make Changelog original made by izdubar for Mercurial
# Update for GIT by Caio99BR <caiooliveirafarias0@gmail.com>

(cat << EOF) > /tmp/DETAILED_CHANGELOG
-----------------------------------------------------------------------------

  Copyright 2009-2010, Ifcaro & jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.

-----------------------------------------------------------------------------

Open PS2 Loader detailed ChangeLog:

EOF

while true
do
# Check if it have .git folder
if ! [ -d .git ]
then
 echo "No .git folder found, exiting..."
 break
fi

# Store author, commit and date on temp file
git log --pretty=format:"%cn - %s - %cd" > /tmp/commit_summary
if ! [ "${?}" == "0" ]
then
 echo "Git command failed, exiting..."
 break
fi

# Hack for fix first commit not showed
printf '\n' >> /tmp/commit_summary

# Store number of commits
number_commits=$(cat /tmp/commit_summary | wc -l)

# Store each commit in one variable[list]
number_summary=0
while read line_commit_summary
do
 number_summary=$((${number_summary} + 1))
 commit_summary[${number_summary}]="commit${number_commits} - ${line_commit_summary}"
 number_commits=$((${number_commits} - 1))
done < /tmp/commit_summary

printf "Found ${number_summary} commits... "

for ((current=1; current <= ${number_summary}; current++))
do
 echo "${commit_summary[${current}]}" >> /tmp/DETAILED_CHANGELOG
done

mv /tmp/DETAILED_CHANGELOG DETAILED_CHANGELOG

rm -fr /tmp/commit_summary

printf "DETAILED_CHANGELOG file created.\n"

# Exit
break
done
