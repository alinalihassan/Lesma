window.BENCHMARK_DATA = {
  "lastUpdate": 1646594104308,
  "repoUrl": "https://github.com/alinalihassan/Lesma",
  "entries": {
    "Catch2 Benchmark": [
      {
        "commit": {
          "author": {
            "name": "alinalihassan",
            "username": "alinalihassan"
          },
          "committer": {
            "name": "alinalihassan",
            "username": "alinalihassan"
          },
          "id": "1345a50ad9833d24d07a41e71c4fa57eda71aae2",
          "message": "Continuous Benchmark CI configuration",
          "timestamp": "2022-03-02T12:10:53Z",
          "url": "https://github.com/alinalihassan/Lesma/pull/1/commits/1345a50ad9833d24d07a41e71c4fa57eda71aae2"
        },
        "date": 1646594103135,
        "tool": "catch2",
        "benches": [
          {
            "name": "Lexer",
            "value": 3.69627,
            "range": "± 459.038",
            "unit": "us",
            "extra": "100 samples\n9 iterations"
          },
          {
            "name": "Parser",
            "value": 2.60963,
            "range": "± 329.349",
            "unit": "us",
            "extra": "100 samples\n10 iterations"
          },
          {
            "name": "Codegen",
            "value": 58.6428,
            "range": "± 20.8994",
            "unit": "us",
            "extra": "100 samples\n1 iterations"
          },
          {
            "name": "Codegen & Optimize",
            "value": 934.621,
            "range": "± 152.691",
            "unit": "us",
            "extra": "100 samples\n1 iterations"
          },
          {
            "name": "Codegen & JIT",
            "value": 1.72118,
            "range": "± 246.213",
            "unit": "ms",
            "extra": "100 samples\n1 iterations"
          },
          {
            "name": "Codegen, Optimize & JIT",
            "value": 2.35841,
            "range": "± 172.562",
            "unit": "ms",
            "extra": "100 samples\n1 iterations"
          }
        ]
      }
    ]
  }
}