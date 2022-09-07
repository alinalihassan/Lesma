enum EvalEventKind {
  Stdout = 'stdout',
  Stderr = 'stderr'
}

interface EvalEvent {
  Message: string
  Kind: EvalEventKind
  Delay: number
}

interface RunResponse {
  formatted?: string | null
  events: EvalEvent[]
}

let lastTime;

function elapsed() {
  if (!lastTime) {
    lastTime = new Date();
    return 0 + ' ms';
  }
  const newTime = new Date();
  const res = newTime.getTime() - lastTime.getTime();
  lastTime = newTime;
  return res + ' ms';
}

// Example POST method implementation:
async function runRequest(query: string) {
  // Default options are marked with *
  const response = await fetch("http://34.140.31.253:18080/api/run", {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({"body": query})
  });
  let json: RunResponse = await response.json();
  return json.events[0].Message; // parses JSON response into native JavaScript objects
}

async function runLesma(source) {
  console.log("Started running");
  elapsed();

  let output = await runRequest(source);

  console.log(elapsed(), 'running lesma');
  return output;
}

onmessage = async function(e) {
  const [tag, source] = e.data;
  switch (tag) {
    case 'run':
      const result = await runLesma(source);
      postMessage(['runResult', result]);
      break;
  }
};
