#!/bin/sh

# exit value 1, print statement for missing parameters
if [ ! $# -eq 2 ]
then
    echo "ERROR: Invalid number of arugments $#"
    echo "Usage: finder.sh /path/to/directory <search_string>"
    exit 1
fi

# first argument is a path to directoy on filesystem
filesdir=$1

# second argument is text string to be search for within files
searchstr=$2

# exit value 1, print statement if directory is invalid
if [ ! -d $filesdir ]
then
    echo "ERROR: Directory does not exist: $filesdir"
    exit 1
fi

cd $filesdir
num_files=`find . -type f | wc -l`
num_lines=`grep $searchstr * -r | wc -l`

echo "The number of files are $num_files and the number of matching lines are $num_lines"
