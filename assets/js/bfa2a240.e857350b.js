"use strict";(self.webpackChunklesma_website=self.webpackChunklesma_website||[]).push([[1020],{3905:function(e,n,t){t.d(n,{Zo:function(){return u},kt:function(){return d}});var a=t(7294);function r(e,n,t){return n in e?Object.defineProperty(e,n,{value:t,enumerable:!0,configurable:!0,writable:!0}):e[n]=t,e}function i(e,n){var t=Object.keys(e);if(Object.getOwnPropertySymbols){var a=Object.getOwnPropertySymbols(e);n&&(a=a.filter((function(n){return Object.getOwnPropertyDescriptor(e,n).enumerable}))),t.push.apply(t,a)}return t}function o(e){for(var n=1;n<arguments.length;n++){var t=null!=arguments[n]?arguments[n]:{};n%2?i(Object(t),!0).forEach((function(n){r(e,n,t[n])})):Object.getOwnPropertyDescriptors?Object.defineProperties(e,Object.getOwnPropertyDescriptors(t)):i(Object(t)).forEach((function(n){Object.defineProperty(e,n,Object.getOwnPropertyDescriptor(t,n))}))}return e}function s(e,n){if(null==e)return{};var t,a,r=function(e,n){if(null==e)return{};var t,a,r={},i=Object.keys(e);for(a=0;a<i.length;a++)t=i[a],n.indexOf(t)>=0||(r[t]=e[t]);return r}(e,n);if(Object.getOwnPropertySymbols){var i=Object.getOwnPropertySymbols(e);for(a=0;a<i.length;a++)t=i[a],n.indexOf(t)>=0||Object.prototype.propertyIsEnumerable.call(e,t)&&(r[t]=e[t])}return r}var l=a.createContext({}),c=function(e){var n=a.useContext(l),t=n;return e&&(t="function"==typeof e?e(n):o(o({},n),e)),t},u=function(e){var n=c(e.components);return a.createElement(l.Provider,{value:n},e.children)},p={inlineCode:"code",wrapper:function(e){var n=e.children;return a.createElement(a.Fragment,{},n)}},m=a.forwardRef((function(e,n){var t=e.components,r=e.mdxType,i=e.originalType,l=e.parentName,u=s(e,["components","mdxType","originalType","parentName"]),m=c(t),d=r,f=m["".concat(l,".").concat(d)]||m[d]||p[d]||i;return t?a.createElement(f,o(o({ref:n},u),{},{components:t})):a.createElement(f,o({ref:n},u))}));function d(e,n){var t=arguments,r=n&&n.mdxType;if("string"==typeof e||r){var i=t.length,o=new Array(i);o[0]=m;var s={};for(var l in n)hasOwnProperty.call(n,l)&&(s[l]=n[l]);s.originalType=e,s.mdxType="string"==typeof e?e:r,o[1]=s;for(var c=2;c<i;c++)o[c]=t[c];return a.createElement.apply(null,o)}return a.createElement.apply(null,t)}m.displayName="MDXCreateElement"},9130:function(e,n,t){t.r(n),t.d(n,{assets:function(){return u},contentTitle:function(){return l},default:function(){return d},frontMatter:function(){return s},metadata:function(){return c},toc:function(){return p}});var a=t(3117),r=t(102),i=(t(7294),t(3905)),o=["components"],s={id:"functions",title:"Functions",sidebar_position:4},l="Functions",c={unversionedId:"common/functions",id:"common/functions",title:"Functions",description:"Functions are the basic building blocks of Lesma, and many times they are used behind the scenes,",source:"@site/docs/common/functions.md",sourceDirName:"common",slug:"/common/functions",permalink:"/docs/common/functions",draft:!1,editUrl:"https://github.com/alinalihassan/Lesma/tree/main/docs/docs/common/functions.md",tags:[],version:"current",sidebarPosition:4,frontMatter:{id:"functions",title:"Functions",sidebar_position:4},sidebar:"tutorialSidebar",previous:{title:"Types",permalink:"/docs/common/types"},next:{title:"Comments",permalink:"/docs/common/comments"}},u={},p=[{value:"Parameters",id:"parameters",level:2},{value:"Return values",id:"return-values",level:2},{value:"Extern functions",id:"extern-functions",level:2},{value:"Varargs",id:"varargs",level:2}],m={toc:p};function d(e){var n=e.components,t=(0,r.Z)(e,o);return(0,i.kt)("wrapper",(0,a.Z)({},m,t,{components:n,mdxType:"MDXLayout"}),(0,i.kt)("h1",{id:"functions"},"Functions"),(0,i.kt)("p",null,"Functions are the basic building blocks of Lesma, and many times they are used behind the scenes,\nbut you've also seen one of them, ",(0,i.kt)("inlineCode",{parentName:"p"},"print"),". Here's an example function definition to compute fibbonaci."),(0,i.kt)("pre",null,(0,i.kt)("code",{parentName:"pre",className:"language-python"},"def fibonacci(x: int) -> int\n    if x <= 1\n        return x\n    return fibonacci(x - 1) + fibonacci(x - 2)\n\nprint(fibonacci(8)) # Prints 21\n")),(0,i.kt)("p",null,"We define functions with the keyword ",(0,i.kt)("inlineCode",{parentName:"p"},"def"),", followed by the function name and a set of parenthesis.\nThe body block is then defined by a level of indentation. "),(0,i.kt)("p",null,"We can call any defined function, like ",(0,i.kt)("inlineCode",{parentName:"p"},"print"),", by entering its name followed by a set of parenthesis."),(0,i.kt)("h2",{id:"parameters"},"Parameters"),(0,i.kt)("p",null,"We can define functions to receive parameters when we call them. In the example above,\n",(0,i.kt)("inlineCode",{parentName:"p"},"x")," is our parameter in the function named ",(0,i.kt)("inlineCode",{parentName:"p"},"fibonacci"),". "),(0,i.kt)("p",null,"Unlike variable definitions, function parameters require us to specify the type."),(0,i.kt)("p",null,"We can also define default values for those parameters in a similar way we assign values to variables.\nSimply by adding an ",(0,i.kt)("inlineCode",{parentName:"p"},"=")," and a value after the operator"),(0,i.kt)("div",{className:"admonition admonition-danger alert alert--danger"},(0,i.kt)("div",{parentName:"div",className:"admonition-heading"},(0,i.kt)("h5",{parentName:"div"},(0,i.kt)("span",{parentName:"h5",className:"admonition-icon"},(0,i.kt)("svg",{parentName:"span",xmlns:"http://www.w3.org/2000/svg",width:"12",height:"16",viewBox:"0 0 12 16"},(0,i.kt)("path",{parentName:"svg",fillRule:"evenodd",d:"M5.05.31c.81 2.17.41 3.38-.52 4.31C3.55 5.67 1.98 6.45.9 7.98c-1.45 2.05-1.7 6.53 3.53 7.7-2.2-1.16-2.67-4.52-.3-6.61-.61 2.03.53 3.33 1.94 2.86 1.39-.47 2.3.53 2.27 1.67-.02.78-.31 1.44-1.13 1.81 3.42-.59 4.78-3.42 4.78-5.56 0-2.84-2.53-3.22-1.25-5.61-1.52.13-2.03 1.13-1.89 2.75.09 1.08-1.02 1.8-1.86 1.33-.67-.41-.66-1.19-.06-1.78C8.18 5.31 8.68 2.45 5.05.32L5.03.3l.02.01z"}))),"danger")),(0,i.kt)("div",{parentName:"div",className:"admonition-content"},(0,i.kt)("p",{parentName:"div"},"Default parameter values are not implemented yet. Additionally, string interpolation is not implemented."))),(0,i.kt)("pre",null,(0,i.kt)("code",{parentName:"pre",className:"language-python"},'def hello(name: str = "Mark")\n    print("Hello \\{name}!")\n\nhello() # Prints Hello Mark!\n')),(0,i.kt)("h2",{id:"return-values"},"Return values"),(0,i.kt)("p",null,"Functions can return values to the caller of the function. The type of the return value is\ndefined in the function signature after the parameters, delimited by an arrow (",(0,i.kt)("inlineCode",{parentName:"p"},"->"),").\nValues are returned using the ",(0,i.kt)("inlineCode",{parentName:"p"},"return")," keyword followed by a value of the corresponding type."),(0,i.kt)("div",{className:"admonition admonition-tip alert alert--success"},(0,i.kt)("div",{parentName:"div",className:"admonition-heading"},(0,i.kt)("h5",{parentName:"div"},(0,i.kt)("span",{parentName:"h5",className:"admonition-icon"},(0,i.kt)("svg",{parentName:"span",xmlns:"http://www.w3.org/2000/svg",width:"12",height:"16",viewBox:"0 0 12 16"},(0,i.kt)("path",{parentName:"svg",fillRule:"evenodd",d:"M6.5 0C3.48 0 1 2.19 1 5c0 .92.55 2.25 1 3 1.34 2.25 1.78 2.78 2 4v1h5v-1c.22-1.22.66-1.75 2-4 .45-.75 1-2.08 1-3 0-2.81-2.48-5-5.5-5zm3.64 7.48c-.25.44-.47.8-.67 1.11-.86 1.41-1.25 2.06-1.45 3.23-.02.05-.02.11-.02.17H5c0-.06 0-.13-.02-.17-.2-1.17-.59-1.83-1.45-3.23-.2-.31-.42-.67-.67-1.11C2.44 6.78 2 5.65 2 5c0-2.2 2.02-4 4.5-4 1.22 0 2.36.42 3.22 1.19C10.55 2.94 11 3.94 11 5c0 .66-.44 1.78-.86 2.48zM4 14h5c-.23 1.14-1.3 2-2.5 2s-2.27-.86-2.5-2z"}))),"tip")),(0,i.kt)("div",{parentName:"div",className:"admonition-content"},(0,i.kt)("p",{parentName:"div"},"A function that doesn't have a return type is expected to return nothing."))),(0,i.kt)("h2",{id:"extern-functions"},"Extern functions"),(0,i.kt)("p",null,"Extern functions are functions that we import from the C family of languages. It's Lesma's\n",(0,i.kt)("em",{parentName:"p"},"Foreign Function Interface")," or ",(0,i.kt)("em",{parentName:"p"},"FFI"),"."),(0,i.kt)("p",null,"Let's say we want to get the square root of a number, and we don't know how to implement the function ourselves.\nIf you are sufficiently familiar with C, you can use the math library from there!"),(0,i.kt)("pre",null,(0,i.kt)("code",{parentName:"pre",className:"language-python"},"def extern sqrt(x: float) -> float\n\nprint(sqrt(4.0)) # Prints 2\n")),(0,i.kt)("h2",{id:"varargs"},"Varargs"),(0,i.kt)("p",null,"Extern functions can have a variable amount of arguments, and to support that, you can\nuse an ellipsis(",(0,i.kt)("inlineCode",{parentName:"p"},"..."),"). For example ",(0,i.kt)("inlineCode",{parentName:"p"},"printf")," uses a variable amount of parameters, let's see how\nwe can define that."),(0,i.kt)("pre",null,(0,i.kt)("code",{parentName:"pre",className:"language-python"},'def extern printf(fmt: str, ...)\n\nprintf("Hello %s and %s!\\n", "Mark", "Sussie") # Prints Hello Mark and Sussie!\n')))}d.isMDXComponent=!0}}]);