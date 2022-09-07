let lastTime;

function elapsed() {
  if (!lastTime) {
    lastTime = new Date();
    return 0 + ' ms';
  }
  const newTime = new Date();
  const res = newTime - lastTime;
  lastTime = newTime;
  return res + ' ms';
}

// Example POST method implementation:
async function runRequest(query = "print(\"Hello from Lesma!\")\n") {
  // Default options are marked with *
  const response = await fetch("http://34.140.31.253:18080/api/run", {
    method: 'POST',
    mode: 'cors',
    cache: 'no-cache',
    credentials: 'same-origin', // include, *same-origin, omit
    headers: {
      'Content-Type': 'application/json'
      // 'Content-Type': 'application/x-www-form-urlencoded',
    },
    redirect: 'follow', // manual, *follow, error
    referrerPolicy: 'no-referrer', // no-referrer, *no-referrer-when-downgrade, origin, origin-when-cross-origin, same-origin, strict-origin, strict-origin-when-cross-origin, unsafe-url
    body: JSON.stringify({"body": query}) // body data type must match "Content-Type" header
  });
  let json = await response.json();
  console.log(json);
  console.log(json.events[0].Message);
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
