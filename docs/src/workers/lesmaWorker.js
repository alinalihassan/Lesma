var lastTime;

function elapsed() {
  if (!lastTime) {
    lastTime = new Date();
    return 0 + ' ms';
  }
  var newTime = new Date();
  var res = newTime - lastTime;
  lastTime = newTime;
  return res + ' ms';
}

// Example POST method implementation:
async function runRequest(query = "") {
  // Default options are marked with *
  const response = await fetch("http://34.140.31.253:18080/api/run", {
    method: 'POST',
    mode: 'no-cors',
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
  return response.json(); // parses JSON response into native JavaScript objects
}

async function runLesma(args, source) {
  elapsed();

  let output = await runRequest(source);

  console.log(elapsed(), 'running lesma');
  return output;
}

onmessage = async function(e) {
  const [tag, args, source] = e.data;
  switch (tag) {
    case 'run':
      const result = await runLesma(args, source);
      postMessage(['runResult', result]);
      break;
  }
};
