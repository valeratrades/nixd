# RUN: nixd-attrset-eval --lit-test < %s | FileCheck %s

`attrset/evalString` answers with a string, and — unlike `attrset/evalExpr` —
evaluates into a local value. The attrset held for `attrset/attrpathInfo` and
friends survives it, so a worker can answer these while still serving
completion from what it was given.

```json
{
   "jsonrpc":"2.0",
   "id":0,
   "method":"attrset/evalExpr",
   "params": "{ held = \"still here\"; }"
}
```

```json
{
   "jsonrpc":"2.0",
   "id":1,
   "method":"attrset/evalString",
   "params": "builtins.toJSON [ [ \"outputs\" \"buffer\" ] ]"
}
```

```
CHECK: "id": 1,
CHECK-NEXT:  "jsonrpc": "2.0",
CHECK-NEXT:  "result": "{{\[\[}}\"outputs\",\"buffer\"]]"
```

A failing expression replies with an error, and the message is the evaluator's
own — that is the only channel by which the module system's complaint about a
particular option reaches the controller.

```json
{
   "jsonrpc":"2.0",
   "id":2,
   "method":"attrset/evalString",
   "params": "builtins.deepSeq (throw \"A definition for option `outputs.buffer' is not of type `signed integer'.\") \"\""
}
```

```
CHECK: "error": {
CHECK-NEXT:    "code": -32001,
CHECK-NEXT:    "message": "A definition for option `outputs.buffer' is not of type `signed integer'."
CHECK-NEXT:  },
CHECK-NEXT:  "id": 2,
```

The held value is untouched by both of the above.

```json
{
   "jsonrpc":"2.0",
   "id":3,
   "method":"attrset/attrpathInfo",
   "params": [ "held" ]
}
```

```
CHECK: "id": 3,
CHECK-NEXT:  "jsonrpc": "2.0",
CHECK-NEXT:  "result": {
```

```json
{"jsonrpc":"2.0","method":"exit"}
```
