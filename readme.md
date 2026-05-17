pes2ug21cs278@manish:/mnt/c/Users/manib$ wrk -t4 -c100 -d30s http://192.168.29.36:80/hi
Running 30s test @ http://192.168.29.36:80/hi
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   239.96ms   51.75ms 757.81ms   97.24%
    Req/Sec    59.35     38.12   222.00     65.15%
  6935 requests in 30.08s, 426.67KB read
  Socket errors: connect 0, read 0, write 0, timeout 84
Requests/sec:    230.58
Transfer/sec:     14.19KB