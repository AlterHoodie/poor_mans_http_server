pes2ug21cs278@manish:/mnt/c/Users/manib$ wrk -t4 -c100 -d30s http://192.168.29.12:80/hi
Running 30s test @ http://192.168.29.12:80/hi
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   619.12ms  484.15ms   1.99s    83.74%
    Req/Sec    38.35     34.10   180.00     76.56%
  1790 requests in 30.07s, 110.13KB read
  Socket errors: connect 0, read 0, write 0, timeout 480
Requests/sec:     59.52