#!/usr/bin/bash

# Does a find on $HOME for all vds files and attempts to generate a 
# thumbnail. If the thumbnailer fails, it copies the log to a FAILS
# folder in the current folder.

# move any existing FAILS folder
if [[ -d FAILS ]]; then
  mv FAILS FAILS_`date +%FT%H:%M`
fi

# create FAILS folder or quit
mkdir FAILS || exit 1

# track success vs failure
declare -i succeeded; succeeded=0
declare -i failed; failed=0

# VDS list
vdslist='/tmp/_vdsFiles.txt'
rm $vdslist

# start folder
if [[ ! -z "$1" ]]; then
  folder=$1
else
  folder=$HOME
fi

# find all the vds files under $HOME and pipe to /tmp/_vdsFiles.txt
find $folder -iname '*.vds' -print | sort > $vdslist

# for each vds, run the thumbnailer for the vds file, with thumbnail to /dev/null
while read -r vds; do
  # test with file
  rm -f /tmp/vds-thumbnailer.err
  cmd="vds-thumbnailer \"${vds}\" /dev/null 2>/tmp/vds-thumbnailer.err"
  echo $cmd > /tmp/vds-thumbnailer.cmd
  eval $cmd &> /tmp/vds-thumbnailer.err2

  # check return code
  if [[ "0" -ne "$?" || -s /tmp/vds-thumbnailer.err ]]; then
    # ack!
    echo "vds-thumbnailer '${vds}' /dev/null (FAILED)"
    failed+=1

    # create folder if needed
    if [[ ! -d "./FAILS" ]]; then
      mkdir -p FAILS
    fi

    # copy log file
    vds_filename="${vds##*/}"
    cp /tmp/vds-thumbnailer.log "./FAILS/_${vds_filename}_vds-thumbnailer.log"

    # append command - in case of a crash and there's no actual log
    echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
    echo "### Command:" >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
    cat /tmp/vds-thumbnailer.cmd >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
    echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"

    # append any errors
    if [[ -s /tmp/vds-thumbnailer.err ]]; then
      echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      echo "### stderr capture ###" >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      cat /tmp/vds-thumbnailer.err >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
    fi

    # append any errors (2)
    if [[ -s /tmp/vds-thumbnailer.err2 ]]; then
      echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      echo "### stderr outer capture ###" >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      cat /tmp/vds-thumbnailer.err2 >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
      echo >> "./FAILS/_${vds_filename}_vds-thumbnailer.log"
    fi
  else
    # success
    echo "vds-thumbnailer '${vds}' /dev/null (OK)"
    succeeded+=1
  fi
done <$vdslist

# basic stats
echo "$succeeded succeeded, and $failed failed."
if [[ 0 -ne $failed ]]; then
  echo "See FAILED folder for failure logs."
fi

# grep and move failure reports into category folders

# wait for data failed
mkdir FAILS/WaitForCompletionFailed
grep -le 'Failed' FAILS/_* | xargs -I '{}' mv '{}' FAILS/WaitForCompletionFailed/

# seg faults
mkdir FAILS/SegFault
grep -le 'core dumped' FAILS/_* | xargs -I '{}' mv '{}' FAILS/SegFault/

# asserts
mkdir FAILS/ASSERTs
grep -le 'assert' FAILS/_* | xargs -I '{}' mv '{}' FAILS/ASSERTs/

# unable to find a layer to query
mkdir FAILS/NoChannelLodDimGroup
grep -le 'No suitable' FAILS/_* | xargs -I '{}' mv '{}' FAILS/NoChannelLodDimGroup/

# misnamed files?
mkdir FAILS/NotAHueDataStore
grep -le 'not a Hue' FAILS/_* | xargs -I '{}' mv '{}' FAILS/NotAHueDataStore/

# corrupted vds?
mkdir FAILS/UnableReadFileTable
grep -le 'read file table' FAILS/_* | xargs -I '{}' mv '{}' FAILS/UnableReadFileTable/

# unable to open vds
mkdir FAILS/OpenError
grep -le 'not open VDS' FAILS/_* | xargs -I '{}' mv '{}' FAILS/OpenError/
