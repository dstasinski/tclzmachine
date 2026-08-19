# Contributing

Keep the runtime focused on its primary design goals:

- C implementation
- Tcl 8.6+ embedding
- independent concurrent sessions
- text-only Z-machine operation
- support target of V1-V5, V7 and V8
- intentional exclusion of V6
- IRC/server-oriented request/response execution
- no dependency on an external Z-machine interpreter

Version-dependent behavior should be centralized in helpers rather than scattered through opcode handlers. Opcode work should be accompanied by small focused tests where practical.
