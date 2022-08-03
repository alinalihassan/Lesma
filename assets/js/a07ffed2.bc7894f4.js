"use strict";(self.webpackChunklesma_website=self.webpackChunklesma_website||[]).push([[5747],{3905:(e,t,r)=>{r.d(t,{Zo:()=>p,kt:()=>d});var n=r(7294);function a(e,t,r){return t in e?Object.defineProperty(e,t,{value:r,enumerable:!0,configurable:!0,writable:!0}):e[t]=r,e}function i(e,t){var r=Object.keys(e);if(Object.getOwnPropertySymbols){var n=Object.getOwnPropertySymbols(e);t&&(n=n.filter((function(t){return Object.getOwnPropertyDescriptor(e,t).enumerable}))),r.push.apply(r,n)}return r}function l(e){for(var t=1;t<arguments.length;t++){var r=null!=arguments[t]?arguments[t]:{};t%2?i(Object(r),!0).forEach((function(t){a(e,t,r[t])})):Object.getOwnPropertyDescriptors?Object.defineProperties(e,Object.getOwnPropertyDescriptors(r)):i(Object(r)).forEach((function(t){Object.defineProperty(e,t,Object.getOwnPropertyDescriptor(r,t))}))}return e}function s(e,t){if(null==e)return{};var r,n,a=function(e,t){if(null==e)return{};var r,n,a={},i=Object.keys(e);for(n=0;n<i.length;n++)r=i[n],t.indexOf(r)>=0||(a[r]=e[r]);return a}(e,t);if(Object.getOwnPropertySymbols){var i=Object.getOwnPropertySymbols(e);for(n=0;n<i.length;n++)r=i[n],t.indexOf(r)>=0||Object.prototype.propertyIsEnumerable.call(e,r)&&(a[r]=e[r])}return a}var o=n.createContext({}),c=function(e){var t=n.useContext(o),r=t;return e&&(r="function"==typeof e?e(t):l(l({},t),e)),r},p=function(e){var t=c(e.components);return n.createElement(o.Provider,{value:t},e.children)},u={inlineCode:"code",wrapper:function(e){var t=e.children;return n.createElement(n.Fragment,{},t)}},m=n.forwardRef((function(e,t){var r=e.components,a=e.mdxType,i=e.originalType,o=e.parentName,p=s(e,["components","mdxType","originalType","parentName"]),m=c(r),d=a,b=m["".concat(o,".").concat(d)]||m[d]||u[d]||i;return r?n.createElement(b,l(l({ref:t},p),{},{components:r})):n.createElement(b,l({ref:t},p))}));function d(e,t){var r=arguments,a=t&&t.mdxType;if("string"==typeof e||a){var i=r.length,l=new Array(i);l[0]=m;var s={};for(var o in t)hasOwnProperty.call(t,o)&&(s[o]=t[o]);s.originalType=e,s.mdxType="string"==typeof e?e:a,l[1]=s;for(var c=2;c<i;c++)l[c]=r[c];return n.createElement.apply(null,l)}return n.createElement.apply(null,r)}m.displayName="MDXCreateElement"},9977:(e,t,r)=>{r.r(t),r.d(t,{assets:()=>o,contentTitle:()=>l,default:()=>u,frontMatter:()=>i,metadata:()=>s,toc:()=>c});var n=r(7462),a=(r(7294),r(3905));const i={id:"literals",title:"Literals",sidebar_position:1},l="Literals",s={unversionedId:"basics/literals",id:"basics/literals",title:"Literals",description:"Literals are primitive values like booleans, numbers, etc.",source:"@site/docs/basics/literals.md",sourceDirName:"basics",slug:"/basics/literals",permalink:"/docs/basics/literals",draft:!1,editUrl:"https://github.com/alinalihassan/Lesma/tree/main/docs/docs/basics/literals.md",tags:[],version:"current",sidebarPosition:1,frontMatter:{id:"literals",title:"Literals",sidebar_position:1},sidebar:"tutorialSidebar",previous:{title:"Programming a Guessing Game",permalink:"/docs/introduction/guessing-game"},next:{title:"Variables",permalink:"/docs/basics/variables"}},o={},c=[{value:"Booleans",id:"booleans",level:2},{value:"Numeric",id:"numeric",level:2},{value:"String Literals",id:"string-literals",level:2}],p={toc:c};function u(e){let{components:t,...r}=e;return(0,a.kt)("wrapper",(0,n.Z)({},p,r,{components:t,mdxType:"MDXLayout"}),(0,a.kt)("h1",{id:"literals"},"Literals"),(0,a.kt)("p",null,"Literals are primitive values like booleans, numbers, etc."),(0,a.kt)("h2",{id:"booleans"},"Booleans"),(0,a.kt)("p",null,"Booleans, using the ",(0,a.kt)("inlineCode",{parentName:"p"},"bool")," type, can only two values, ",(0,a.kt)("inlineCode",{parentName:"p"},"true")," or ",(0,a.kt)("inlineCode",{parentName:"p"},"false"),"."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-js"},"let x: bool = true\n")),(0,a.kt)("h2",{id:"numeric"},"Numeric"),(0,a.kt)("p",null,"Numeric literals can be either integers, or floating points, using ",(0,a.kt)("inlineCode",{parentName:"p"},"int")," and ",(0,a.kt)("inlineCode",{parentName:"p"},"float")," types respectively. Currently, Lesma assumes all numeric values to be signed. Both types are 64 bits."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-js"},"let x: int = 5\nlet pi: float = 3.14\n")),(0,a.kt)("h2",{id:"string-literals"},"String Literals"),(0,a.kt)("p",null,"Strings, using the ",(0,a.kt)("inlineCode",{parentName:"p"},"str")," type, are enclosed in double-quotes ",(0,a.kt)("inlineCode",{parentName:"p"},'"'),". They can contain both ASCII and UTF-8 characters."),(0,a.kt)("pre",null,(0,a.kt)("code",{parentName:"pre",className:"language-js"},'let hello: str = "Hello World!"\n')))}u.isMDXComponent=!0}}]);