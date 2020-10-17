# shttproxy

The simple HTTP proxy(pronounced like "shit proxy")

Proxies HTTP request based on the `Host:` header.

Example:

```
shttproxy -h 0.0.0.0 -p 80 www@8080 git@8081 ftp@8082
```

Requests to:

- www.example.com goto example.com:8080
- git.example.com goto example.com:8081
- ftp.example.com goto example.com:8081
