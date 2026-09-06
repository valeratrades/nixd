# RUN: rm -rf %t && mkdir -p %t
# RUN: cp %S/Inputs/schema-lib.nix %t/lib.nix
# RUN: cp %S/Inputs/schema-mod.nix %t/mod.nix
# RUN: sed "s|@DIR@|%t|g" %s > %t/in.md
# RUN: nixd --lit-test --nixpkgs-expr="import %t/lib.nix" < %t/in.md > %t/out.json
# RUN: FileCheck %s --check-prefix=BAD < %t/out.json
# RUN: FileCheck %s --check-prefix=CLEAN < %t/out.json

A document naming its own option module is evaluated against it, and values that
do not typecheck are reported where they are written. Both failing options are
reported, not just the first.

The eight completions below are a barrier, not filler. Publishing a diagnostic
is a notification, so nothing in the transcript blocks on it; but the server
does not exit until its thread pool drains, and each completion holds a pool
thread across a round trip to the very worker the validation is queued behind.
Two round trips are needed (which options fail, then why); eight are bought.

<-- initialize(0)

```json
{
   "jsonrpc":"2.0",
   "id":0,
   "method":"initialize",
   "params":{
      "processId":123,
      "rootPath":"",
      "capabilities":{
      },
      "trace":"off"
   }
}
```


<-- textDocument/didOpen


```nix file://@DIR@/config.nix
#:schema ./mod.nix
{
  count = 1;
  outputs = {
    buffer = "x";
    name = 2;
  };
}
```

`buffer` and `name` are each underlined by their own name token, and each
carries the module system's own complaint about that option.

```
     BAD: "code": "outputs.buffer",
BAD-NEXT:  "message": "A definition for option `outputs.buffer' is not of type `signed integer'.",
BAD-NEXT:  "range": {
BAD-NEXT:    "end": {
BAD-NEXT:      "character": 10,
BAD-NEXT:      "line": 4
BAD-NEXT:    },
BAD-NEXT:    "start": {
BAD-NEXT:      "character": 4,
BAD-NEXT:      "line": 4
BAD-NEXT:    }
BAD-NEXT:  },
BAD-NEXT:  "severity": 1,
BAD-NEXT:  "source": "nixd-eval"
BAD-NEXT: },
BAD-NEXT: {
BAD-NEXT:  "code": "outputs.name",
BAD-NEXT:  "message": "A definition for option `outputs.name' is not of type `string'.",
BAD-NEXT:  "range": {
BAD-NEXT:    "end": {
BAD-NEXT:      "character": 8,
BAD-NEXT:      "line": 5
BAD-NEXT:    },
BAD-NEXT:    "start": {
BAD-NEXT:      "character": 4,
BAD-NEXT:      "line": 5
BAD-NEXT:    }
BAD-NEXT:  },
BAD-NEXT:  "severity": 1,
BAD-NEXT:  "source": "nixd-eval"
```

A document with no directive is not a module as far as nixd knows, and is left
alone.

```nix file://@DIR@/plain.nix
{ buffer = "x"; }
```

Both documents are opened on the input thread, in transcript order, so the
first publish past `config.nix`'s own is `plain.nix`'s.

```
     CLEAN: "uri": "{{.*}}/config.nix",
     CLEAN: "diagnostics": [],
CLEAN-NEXT:  "uri": "{{.*}}/plain.nix",
```

```json
{"jsonrpc":"2.0","id":101,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```
```json
{"jsonrpc":"2.0","id":102,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```
```json
{"jsonrpc":"2.0","id":103,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```
```json
{"jsonrpc":"2.0","id":104,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```
```json
{"jsonrpc":"2.0","id":105,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```
```json
{"jsonrpc":"2.0","id":106,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```
```json
{"jsonrpc":"2.0","id":107,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```
```json
{"jsonrpc":"2.0","id":108,"method":"textDocument/completion","params":{"textDocument":{"uri":"file://@DIR@/config.nix"},"position":{"line":2,"character":4},"context":{"triggerKind":1}}}
```

```json
{"jsonrpc":"2.0","method":"exit"}
```
