# Multithreaded Proxy
## Technical specification
Implement a caching HTTP proxy with a cache in RAM. 
The proxy must be implemented as a single process. 
For each incoming HTTP connection, you need to create a thread. 
If it is impossible to create a thread, it is allowed to block incoming connections or return an error.
The proxy must provide simultaneous operation of several clients.
