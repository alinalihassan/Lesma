"use strict";(self.webpackChunklesma_website=self.webpackChunklesma_website||[]).push([[3420],{3905:(e,t,n)=>{n.d(t,{Zo:()=>p,kt:()=>c});var r=n(7294);function a(e,t,n){return t in e?Object.defineProperty(e,t,{value:n,enumerable:!0,configurable:!0,writable:!0}):e[t]=n,e}function o(e,t){var n=Object.keys(e);if(Object.getOwnPropertySymbols){var r=Object.getOwnPropertySymbols(e);t&&(r=r.filter((function(t){return Object.getOwnPropertyDescriptor(e,t).enumerable}))),n.push.apply(n,r)}return n}function s(e){for(var t=1;t<arguments.length;t++){var n=null!=arguments[t]?arguments[t]:{};t%2?o(Object(n),!0).forEach((function(t){a(e,t,n[t])})):Object.getOwnPropertyDescriptors?Object.defineProperties(e,Object.getOwnPropertyDescriptors(n)):o(Object(n)).forEach((function(t){Object.defineProperty(e,t,Object.getOwnPropertyDescriptor(n,t))}))}return e}function i(e,t){if(null==e)return{};var n,r,a=function(e,t){if(null==e)return{};var n,r,a={},o=Object.keys(e);for(r=0;r<o.length;r++)n=o[r],t.indexOf(n)>=0||(a[n]=e[n]);return a}(e,t);if(Object.getOwnPropertySymbols){var o=Object.getOwnPropertySymbols(e);for(r=0;r<o.length;r++)n=o[r],t.indexOf(n)>=0||Object.prototype.propertyIsEnumerable.call(e,n)&&(a[n]=e[n])}return a}var l=r.createContext({}),u=function(e){var t=r.useContext(l),n=t;return e&&(n="function"==typeof e?e(t):s(s({},t),e)),n},p=function(e){var t=u(e.components);return r.createElement(l.Provider,{value:t},e.children)},m={inlineCode:"code",wrapper:function(e){var t=e.children;return r.createElement(r.Fragment,{},t)}},g=r.forwardRef((function(e,t){var n=e.components,a=e.mdxType,o=e.originalType,l=e.parentName,p=i(e,["components","mdxType","originalType","parentName"]),g=u(n),c=a,d=g["".concat(l,".").concat(c)]||g[c]||m[c]||o;return n?r.createElement(d,s(s({ref:t},p),{},{components:n})):r.createElement(d,s({ref:t},p))}));function c(e,t){var n=arguments,a=t&&t.mdxType;if("string"==typeof e||a){var o=n.length,s=new Array(o);s[0]=g;var i={};for(var l in t)hasOwnProperty.call(t,l)&&(i[l]=t[l]);i.originalType=e,i.mdxType="string"==typeof e?e:a,s[1]=i;for(var u=2;u<o;u++)s[u]=n[u];return r.createElement.apply(null,s)}return r.createElement.apply(null,n)}g.displayName="MDXCreateElement"},2970:(e,t,n)=>{n.r(t),n.d(t,{assets:()=>l,contentTitle:()=>s,default:()=>m,frontMatter:()=>o,metadata:()=>i,toc:()=>u});var r=n(7462),a=(n(7294),n(3905));const o={id:"guessing-game",title:"Programming a Guessing Game",sidebar_position:4},s="Programming a Guessing Game",i={unversionedId:"introduction/guessing-game",id:"introduction/guessing-game",title:"Programming a Guessing Game",description:"Now that you should have everything set up and you wrote your first Hello World example in Lesma, let's build something more challenging.",source:"@site/docs/introduction/guessing-game.md",sourceDirName:"introduction",slug:"/introduction/guessing-game",permalink:"/docs/introduction/guessing-game",draft:!1,editUrl:"https://github.com/alinalihassan/Lesma/tree/main/docs/docs/introduction/guessing-game.md",tags:[],version:"current",sidebarPosition:4,frontMatter:{id:"guessing-game",title:"Programming a Guessing Game",sidebar_position:4},sidebar:"tutorialSidebar",previous:{title:"Hello World!",permalink:"/docs/introduction/hello-world"},next:{title:"Literals",permalink:"/docs/common/literals"}},l={},u=[{value:"Writing the game",id:"writing-the-game",level:2},{value:"Final Result",id:"final-result",level:2}],p={toc:u};function m(e){let{components:t,...n}=e;return(0,a.kt)("wrapper",(0,r.Z)({},p,n,{components:t,mdxType:"MDXLayout"}),(0,a.kt)("h1",{id:"programming-a-guessing-game"},"Programming a Guessing Game"),(0,a.kt)("p",null,"Now that you should have everything set up and you wrote your first Hello World example in Lesma, let's build something more challenging.\nWe'll write a guessing game together. The rules are simple, the program will generate a number between 1 and 100,\nand it will prompt the player to guess the number. If it's not the right number, we'll give him a hint if it's lower or higher.\nIf the guess is correct, it will congratulate the player and exit."),(0,a.kt)("h2",{id:"writing-the-game"},"Writing the game"),(0,a.kt)("p",null,"Make a new source file called ",(0,a.kt)("inlineCode",{parentName:"p"},"guess.les"),". There are quite a few functions from the standard library that we want to use.\nFirst, let's say hi to the player once he starts the game. To do that, we'll use the ",(0,a.kt)("inlineCode",{parentName:"p"},"print")," function."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-python"},'print("Guess the number! \ud83c\udfb0")\n')),(0,a.kt)("p",null,"Afterwards, we would like to decide on a random number, but if we just choose a constant,\nit will be the same value every game, not fun. Let's use the ",(0,a.kt)("inlineCode",{parentName:"p"},"random")," function. This function\ntakes two numbers as inputs, the lower bound and the upper bound."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-typescript"},"let secret_number = random(1, 101)\n")),(0,a.kt)("p",null,"We can now focus on the game loop, and we need to ask the user for input.\nWe will use the ",(0,a.kt)("inlineCode",{parentName:"p"},"input")," function from standard library, which will prompt the user for an input,\nand return it to us."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-typescript"},'while true\n    let guess: str = input("Please input your guess: ")\n')),(0,a.kt)("p",null,"Don't worry about the infinite loop, we'll get there. Our problem is that ",(0,a.kt)("inlineCode",{parentName:"p"},"guess")," in the above example is a string,\nand we can't compare that in the same way we compare numbers, so we'll need to convert it to an integer."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-typescript"},"    let guessed_number = strToInt(guess)\n")),(0,a.kt)("p",null,"Now for the last part, we need to compare the player's guess to our secret number. We can do that\nwith ",(0,a.kt)("inlineCode",{parentName:"p"},"if")," and ",(0,a.kt)("inlineCode",{parentName:"p"},"else")," statements."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-typescript"},'    if guessed_number > secret_number\n        print("Too big!")\n    else if guessed_number < secret_number\n        print("Too small")\n    else\n        print("You win!")\n        break\n')),(0,a.kt)("h2",{id:"final-result"},"Final Result"),(0,a.kt)("p",null,"If you managed to follow along, you should have written code similar to the one below:"),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-typescript"},'print("Guess the number! \ud83c\udfb0")\n\nlet secret_number = random(1, 101)\n\nwhile true\n    let guess = input("Please input your guess: ")\n    let guessed_number = strToInt(guess)\n\n    if guessed_number > secret_number\n        print("Too big!")\n    else if guessed_number < secret_number\n        print("Too small")\n    else\n        print("You win!")\n        break\n')),(0,a.kt)("p",null,"We can run that using Lesma's CLI. Let's give it a shot and see if our game is working."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-shell"},"lesma run guess.les\n")),(0,a.kt)("p",null,"Here's my trial against the robot."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-shell"},"Guess the number! \ud83c\udfb0\nPlease input your guess: 6\nToo small\nPlease input your guess: 47\nToo small\nPlease input your guess: 87\nToo big!\nPlease input your guess: 66\nToo small\nPlease input your guess: 77\nToo small\nPlease input your guess: 80\nToo small\nPlease input your guess: 82\nToo big!\nPlease input your guess: 81\nYou win!\n")))}m.isMDXComponent=!0}}]);