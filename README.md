# co-curl
Concurrent cURL (Version 1.0.0)


```txt
Usage: co-curl [OPTIONS...] <url> 
Download a single file from <url> concurrently 
by splitting it into parts then merge.

OPTIONS:
  -nth, --num-thread <num>   specify the number of threads to be used
  -np, --num-part <num>      set the number of parts of the file
  -cs, --chunk-size <MB>     set downloaded chunk size
  -s, --single-part <index>  download the specified part then exit
  -m, --merge                merge parts then exit
  -o, --output <filename>    output filename
  -u, --username <username>  pass username for identification
  -p, --password <password>  pass password for identification
  -v, --verbose              verbose messages
  -h, --help                 print this usage

  NOTE: --num-part and --chunk-size are mutually execlusive, the lastest takes effect.
  NOTE: --single-part and --merge are mutually execlusive, the lastest takes effect.
```

Usage: 
- use in replacement of curl, wget, ... commands when downloading very large dataset
- to fully utilize available internet bandwith
- and significantly reduce overall tansfer time

Requirement: 
libcurl (curl.h, libcurl.so.x, ...)

Known limitation:
1. Silently fail when data servers do not support partial download or other network issues. Need to use --verbose explicitly to see why it fails.
