"use strict";
(self["webpackChunklesma_website"] = self["webpackChunklesma_website"] || []).push([[3237],{

/***/ 13808:
/***/ ((__unused_webpack_module, __webpack_exports__, __webpack_require__) => {

// ESM COMPAT FLAG
__webpack_require__.r(__webpack_exports__);

// EXPORTS
__webpack_require__.d(__webpack_exports__, {
  "default": () => (/* binding */ Home)
});

// EXTERNAL MODULE: ./node_modules/react/index.js
var react = __webpack_require__(67294);
// EXTERNAL MODULE: ./node_modules/clsx/dist/clsx.m.js
var clsx_m = __webpack_require__(86010);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/Layout/index.js + 69 modules
var Layout = __webpack_require__(67767);
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/Link.js + 1 modules
var Link = __webpack_require__(39960);
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/useDocusaurusContext.js
var useDocusaurusContext = __webpack_require__(52263);
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/useBaseUrl.js
var useBaseUrl = __webpack_require__(44996);
;// CONCATENATED MODULE: ./src/pages/index.module.css
// extracted by mini-css-extract-plugin
/* harmony default export */ const index_module = ({"heroBanner":"heroBanner_qdFl","buttons":"buttons_AeoN","heroLogoWrapper":"heroLogoWrapper_dQoY","heroLogo":"heroLogo_U6bI","heroButtons":"heroButtons_r52D"});
;// CONCATENATED MODULE: ./src/pages/index.tsx
const Button=_ref=>{let{children,href}=_ref;return/*#__PURE__*/react.createElement("div",{className:"col col--2 margin--sm"},/*#__PURE__*/react.createElement(Link/* default */.Z,{className:"button button--outline button--primary button--lg",to:href},children));};function HomepageHeader(){const{siteConfig}=(0,useDocusaurusContext/* default */.Z)();return/*#__PURE__*/react.createElement("header",{className:(0,clsx_m/* default */.Z)('hero',index_module.heroBanner)},/*#__PURE__*/react.createElement("div",{className:"container text--center"},/*#__PURE__*/react.createElement("div",{className:index_module.heroLogoWrapper},/*#__PURE__*/react.createElement("img",{className:index_module.heroLogo,src:(0,useBaseUrl/* default */.Z)('img/logo.svg'),alt:"Lesma Logo"})),/*#__PURE__*/react.createElement("h2",{className:(0,clsx_m/* default */.Z)('hero__title',index_module.heroTitle)},siteConfig.title),/*#__PURE__*/react.createElement("p",{className:"hero__subtitle"},siteConfig.tagline),/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)(index_module.heroButtons,'name','margin-vert--md')},/*#__PURE__*/react.createElement(Button,{href:(0,useBaseUrl/* default */.Z)('docs/introduction/getting-started')},"Get Started"),/*#__PURE__*/react.createElement(Button,{href:(0,useBaseUrl/* default */.Z)('docs/introduction/what-is-lesma')},"Learn More"))));}function Home(){const{siteConfig}=(0,useDocusaurusContext/* default */.Z)();return/*#__PURE__*/react.createElement(Layout/* default */.Z,{title:"Lesma Programming Language",description:"Documentation for Lesma Programming Language"},/*#__PURE__*/react.createElement(HomepageHeader,null),/*#__PURE__*/react.createElement("main",null));}

/***/ })

}]);