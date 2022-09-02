"use strict";
(self["webpackChunklesma_website"] = self["webpackChunklesma_website"] || []).push([[6048],{

/***/ 39058:
/***/ ((__unused_webpack_module, __webpack_exports__, __webpack_require__) => {


// EXPORTS
__webpack_require__.d(__webpack_exports__, {
  "Z": () => (/* binding */ BlogLayout)
});

// EXTERNAL MODULE: ./node_modules/react/index.js
var react = __webpack_require__(67294);
// EXTERNAL MODULE: ./node_modules/clsx/dist/clsx.m.js
var clsx_m = __webpack_require__(86010);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/Layout/index.js + 69 modules
var Layout = __webpack_require__(67767);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-common/lib/hooks/useWindowSize.js
var useWindowSize = __webpack_require__(87524);
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/Link.js + 1 modules
var Link = __webpack_require__(39960);
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/Translate.js + 1 modules
var Translate = __webpack_require__(95999);
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogSidebar/Desktop/styles.module.css
// extracted by mini-css-extract-plugin
/* harmony default export */ const styles_module = ({"sidebar":"sidebar_re4s","sidebarItemTitle":"sidebarItemTitle_pO2u","sidebarItemList":"sidebarItemList_Yudw","sidebarItem":"sidebarItem__DBe","sidebarItemLink":"sidebarItemLink_mo7H","sidebarItemLinkActive":"sidebarItemLinkActive_I1ZP"});
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogSidebar/Desktop/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogSidebarDesktop(_ref){let{sidebar}=_ref;return/*#__PURE__*/react.createElement("aside",{className:"col col--3"},/*#__PURE__*/react.createElement("nav",{className:(0,clsx_m/* default */.Z)(styles_module.sidebar,'thin-scrollbar'),"aria-label":(0,Translate/* translate */.I)({id:'theme.blog.sidebar.navAriaLabel',message:'Blog recent posts navigation',description:'The ARIA label for recent posts in the blog sidebar'})},/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)(styles_module.sidebarItemTitle,'margin-bottom--md')},sidebar.title),/*#__PURE__*/react.createElement("ul",{className:(0,clsx_m/* default */.Z)(styles_module.sidebarItemList,'clean-list')},sidebar.items.map(item=>/*#__PURE__*/react.createElement("li",{key:item.permalink,className:styles_module.sidebarItem},/*#__PURE__*/react.createElement(Link/* default */.Z,{isNavLink:true,to:item.permalink,className:styles_module.sidebarItemLink,activeClassName:styles_module.sidebarItemLinkActive},item.title))))));}
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-common/lib/contexts/navbarSecondaryMenu/content.js
var content = __webpack_require__(13102);
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogSidebar/Mobile/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogSidebarMobileSecondaryMenu(_ref){let{sidebar}=_ref;return/*#__PURE__*/react.createElement("ul",{className:"menu__list"},sidebar.items.map(item=>/*#__PURE__*/react.createElement("li",{key:item.permalink,className:"menu__list-item"},/*#__PURE__*/react.createElement(Link/* default */.Z,{isNavLink:true,to:item.permalink,className:"menu__link",activeClassName:"menu__link--active"},item.title))));}function BlogSidebarMobile(props){return/*#__PURE__*/react.createElement(content/* NavbarSecondaryMenuFiller */.Zo,{component:BlogSidebarMobileSecondaryMenu,props:props});}
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogSidebar/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogSidebar(_ref){let{sidebar}=_ref;const windowSize=(0,useWindowSize/* useWindowSize */.i)();if(!(sidebar!=null&&sidebar.items.length)){return null;}// Mobile sidebar doesn't need to be server-rendered
if(windowSize==='mobile'){return/*#__PURE__*/react.createElement(BlogSidebarMobile,{sidebar:sidebar});}return/*#__PURE__*/react.createElement(BlogSidebarDesktop,{sidebar:sidebar});}
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogLayout/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogLayout(props){const{sidebar,toc,children,...layoutProps}=props;const hasSidebar=sidebar&&sidebar.items.length>0;return/*#__PURE__*/react.createElement(Layout/* default */.Z,layoutProps,/*#__PURE__*/react.createElement("div",{className:"container margin-vert--lg"},/*#__PURE__*/react.createElement("div",{className:"row"},/*#__PURE__*/react.createElement(BlogSidebar,{sidebar:sidebar}),/*#__PURE__*/react.createElement("main",{className:(0,clsx_m/* default */.Z)('col',{'col--7':hasSidebar,'col--9 col--offset-1':!hasSidebar}),itemScope:true,itemType:"http://schema.org/Blog"},children),toc&&/*#__PURE__*/react.createElement("div",{className:"col col--2"},toc))));}

/***/ }),

/***/ 30390:
/***/ ((__unused_webpack_module, __webpack_exports__, __webpack_require__) => {


// EXPORTS
__webpack_require__.d(__webpack_exports__, {
  "Z": () => (/* binding */ BlogPostItem)
});

// EXTERNAL MODULE: ./node_modules/react/index.js
var react = __webpack_require__(67294);
// EXTERNAL MODULE: ./node_modules/clsx/dist/clsx.m.js
var clsx_m = __webpack_require__(86010);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-common/lib/contexts/blogPost.js
var blogPost = __webpack_require__(9460);
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/useBaseUrl.js
var useBaseUrl = __webpack_require__(44996);
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Container/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogPostItemContainer(_ref){var _assets$image;let{children,className}=_ref;const{frontMatter,assets}=(0,blogPost/* useBlogPost */.C)();const{withBaseUrl}=(0,useBaseUrl/* useBaseUrlUtils */.C)();const image=(_assets$image=assets.image)!=null?_assets$image:frontMatter.image;return/*#__PURE__*/react.createElement("article",{className:className,itemProp:"blogPost",itemScope:true,itemType:"http://schema.org/BlogPosting"},image&&/*#__PURE__*/react.createElement("meta",{itemProp:"image",content:withBaseUrl(image,{absolute:true})}),children);}
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/Link.js + 1 modules
var Link = __webpack_require__(39960);
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/Title/styles.module.css
// extracted by mini-css-extract-plugin
/* harmony default export */ const styles_module = ({"title":"title_f1Hy"});
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/Title/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogPostItemHeaderTitle(_ref){let{className}=_ref;const{metadata,isBlogPostPage}=(0,blogPost/* useBlogPost */.C)();const{permalink,title}=metadata;const TitleHeading=isBlogPostPage?'h1':'h2';return/*#__PURE__*/react.createElement(TitleHeading,{className:(0,clsx_m/* default */.Z)(styles_module.title,className),itemProp:"headline"},isBlogPostPage?title:/*#__PURE__*/react.createElement(Link/* default */.Z,{itemProp:"url",to:permalink},title));}
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/Translate.js + 1 modules
var Translate = __webpack_require__(95999);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-common/lib/utils/usePluralForm.js
var usePluralForm = __webpack_require__(88824);
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/Info/styles.module.css
// extracted by mini-css-extract-plugin
/* harmony default export */ const Info_styles_module = ({"container":"container_mt6G"});
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/Info/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */// Very simple pluralization: probably good enough for now
function useReadingTimePlural(){const{selectMessage}=(0,usePluralForm/* usePluralForm */.c)();return readingTimeFloat=>{const readingTime=Math.ceil(readingTimeFloat);return selectMessage(readingTime,(0,Translate/* translate */.I)({id:'theme.blog.post.readingTime.plurals',description:'Pluralized label for "{readingTime} min read". Use as much plural forms (separated by "|") as your language support (see https://www.unicode.org/cldr/cldr-aux/charts/34/supplemental/language_plural_rules.html)',message:'One min read|{readingTime} min read'},{readingTime}));};}function ReadingTime(_ref){let{readingTime}=_ref;const readingTimePlural=useReadingTimePlural();return/*#__PURE__*/react.createElement(react.Fragment,null,readingTimePlural(readingTime));}function Info_Date(_ref2){let{date,formattedDate}=_ref2;return/*#__PURE__*/react.createElement("time",{dateTime:date,itemProp:"datePublished"},formattedDate);}function Spacer(){return/*#__PURE__*/react.createElement(react.Fragment,null,' · ');}function BlogPostItemHeaderInfo(_ref3){let{className}=_ref3;const{metadata}=(0,blogPost/* useBlogPost */.C)();const{date,formattedDate,readingTime}=metadata;return/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)(Info_styles_module.container,'margin-vert--md',className)},/*#__PURE__*/react.createElement(Info_Date,{date:date,formattedDate:formattedDate}),typeof readingTime!=='undefined'&&/*#__PURE__*/react.createElement(react.Fragment,null,/*#__PURE__*/react.createElement(Spacer,null),/*#__PURE__*/react.createElement(ReadingTime,{readingTime:readingTime})));}
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/Author/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function MaybeLink(props){if(props.href){return/*#__PURE__*/react.createElement(Link/* default */.Z,props);}return/*#__PURE__*/react.createElement(react.Fragment,null,props.children);}function BlogPostItemHeaderAuthor(_ref){let{author,className}=_ref;const{name,title,url,imageURL,email}=author;const link=url||email&&"mailto:"+email||undefined;return/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)('avatar margin-bottom--sm',className)},imageURL&&/*#__PURE__*/react.createElement(MaybeLink,{href:link,className:"avatar__photo-link"},/*#__PURE__*/react.createElement("img",{className:"avatar__photo",src:imageURL,alt:name})),name&&/*#__PURE__*/react.createElement("div",{className:"avatar__intro",itemProp:"author",itemScope:true,itemType:"https://schema.org/Person"},/*#__PURE__*/react.createElement("div",{className:"avatar__name"},/*#__PURE__*/react.createElement(MaybeLink,{href:link,itemProp:"url"},/*#__PURE__*/react.createElement("span",{itemProp:"name"},name))),title&&/*#__PURE__*/react.createElement("small",{className:"avatar__subtitle",itemProp:"description"},title)));}
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/Authors/styles.module.css
// extracted by mini-css-extract-plugin
/* harmony default export */ const Authors_styles_module = ({"authorCol":"authorCol_Hf19","imageOnlyAuthorRow":"imageOnlyAuthorRow_pa_O","imageOnlyAuthorCol":"imageOnlyAuthorCol_G86a"});
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/Authors/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */// Component responsible for the authors layout
function BlogPostItemHeaderAuthors(_ref){let{className}=_ref;const{metadata:{authors},assets}=(0,blogPost/* useBlogPost */.C)();const authorsCount=authors.length;if(authorsCount===0){return null;}const imageOnly=authors.every(_ref2=>{let{name}=_ref2;return!name;});return/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)('margin-top--md margin-bottom--sm',imageOnly?Authors_styles_module.imageOnlyAuthorRow:'row',className)},authors.map((author,idx)=>{var _assets$authorsImageU;return/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)(!imageOnly&&'col col--6',imageOnly?Authors_styles_module.imageOnlyAuthorCol:Authors_styles_module.authorCol),key:idx},/*#__PURE__*/react.createElement(BlogPostItemHeaderAuthor,{author:{...author,// Handle author images using relative paths
imageURL:(_assets$authorsImageU=assets.authorsImageUrls[idx])!=null?_assets$authorsImageU:author.imageURL}}));}));}
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Header/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogPostItemHeader(){return/*#__PURE__*/react.createElement("header",null,/*#__PURE__*/react.createElement(BlogPostItemHeaderTitle,null),/*#__PURE__*/react.createElement(BlogPostItemHeaderInfo,null),/*#__PURE__*/react.createElement(BlogPostItemHeaderAuthors,null));}
// EXTERNAL MODULE: ./node_modules/@docusaurus/utils-common/lib/index.js
var lib = __webpack_require__(18780);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/MDXContent/index.js + 36 modules
var MDXContent = __webpack_require__(80210);
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Content/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogPostItemContent(_ref){let{children,className}=_ref;const{isBlogPostPage}=(0,blogPost/* useBlogPost */.C)();return/*#__PURE__*/react.createElement("div",{// This ID is used for the feed generation to locate the main content
id:isBlogPostPage?lib.blogPostContainerID:undefined,className:(0,clsx_m/* default */.Z)('markdown',className),itemProp:"articleBody"},/*#__PURE__*/react.createElement(MDXContent/* default */.Z,null,children));}
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/EditThisPage/index.js + 2 modules
var EditThisPage = __webpack_require__(84881);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/TagsListInline/index.js + 1 modules
var TagsListInline = __webpack_require__(71526);
// EXTERNAL MODULE: ./node_modules/@babel/runtime/helpers/esm/extends.js
var esm_extends = __webpack_require__(87462);
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Footer/ReadMoreLink/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function ReadMoreLabel(){return/*#__PURE__*/react.createElement("b",null,/*#__PURE__*/react.createElement(Translate/* default */.Z,{id:"theme.blog.post.readMore",description:"The label used in blog post item excerpts to link to full blog posts"},"Read More"));}function BlogPostItemFooterReadMoreLink(props){const{blogPostTitle,...linkProps}=props;return/*#__PURE__*/react.createElement(Link/* default */.Z,(0,esm_extends/* default */.Z)({"aria-label":(0,Translate/* translate */.I)({message:'Read more about {title}',id:'theme.blog.post.readMoreLabel',description:'The ARIA label for the link to full blog posts from excerpts'},{title:blogPostTitle})},linkProps),/*#__PURE__*/react.createElement(ReadMoreLabel,null));}
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Footer/styles.module.css
// extracted by mini-css-extract-plugin
/* harmony default export */ const Footer_styles_module = ({"blogPostFooterDetailsFull":"blogPostFooterDetailsFull_mRVl"});
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/Footer/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */function BlogPostItemFooter(){const{metadata,isBlogPostPage}=(0,blogPost/* useBlogPost */.C)();const{tags,title,editUrl,hasTruncateMarker}=metadata;// A post is truncated if it's in the "list view" and it has a truncate marker
const truncatedPost=!isBlogPostPage&&hasTruncateMarker;const tagsExists=tags.length>0;const renderFooter=tagsExists||truncatedPost||editUrl;if(!renderFooter){return null;}return/*#__PURE__*/react.createElement("footer",{className:(0,clsx_m/* default */.Z)('row docusaurus-mt-lg',isBlogPostPage&&Footer_styles_module.blogPostFooterDetailsFull)},tagsExists&&/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)('col',{'col--9':truncatedPost})},/*#__PURE__*/react.createElement(TagsListInline/* default */.Z,{tags:tags})),isBlogPostPage&&editUrl&&/*#__PURE__*/react.createElement("div",{className:"col margin-top--sm"},/*#__PURE__*/react.createElement(EditThisPage/* default */.Z,{editUrl:editUrl})),truncatedPost&&/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)('col text--right',{'col--3':tagsExists})},/*#__PURE__*/react.createElement(BlogPostItemFooterReadMoreLink,{blogPostTitle:title,to:metadata.permalink})));}
;// CONCATENATED MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/BlogPostItem/index.js
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */// apply a bottom margin in list view
function useContainerClassName(){const{isBlogPostPage}=(0,blogPost/* useBlogPost */.C)();return!isBlogPostPage?'margin-bottom--xl':undefined;}function BlogPostItem(_ref){let{children,className}=_ref;const containerClassName=useContainerClassName();return/*#__PURE__*/react.createElement(BlogPostItemContainer,{className:(0,clsx_m/* default */.Z)(containerClassName,className)},/*#__PURE__*/react.createElement(BlogPostItemHeader,null),/*#__PURE__*/react.createElement(BlogPostItemContent,null,children),/*#__PURE__*/react.createElement(BlogPostItemFooter,null));}

/***/ }),

/***/ 9460:
/***/ ((__unused_webpack_module, __webpack_exports__, __webpack_require__) => {

/* harmony export */ __webpack_require__.d(__webpack_exports__, {
/* harmony export */   "C": () => (/* binding */ useBlogPost),
/* harmony export */   "n": () => (/* binding */ BlogPostProvider)
/* harmony export */ });
/* harmony import */ var react__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(67294);
/* harmony import */ var _utils_reactUtils__WEBPACK_IMPORTED_MODULE_1__ = __webpack_require__(902);
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */const Context=/*#__PURE__*/react__WEBPACK_IMPORTED_MODULE_0__.createContext(null);/**
 * Note: we don't use `PropBlogPostContent` as context value on purpose. Metadata is
 * currently stored inside the MDX component, but we may want to change that in
 * the future.
 */function useContextValue(_ref){let{content,isBlogPostPage}=_ref;return (0,react__WEBPACK_IMPORTED_MODULE_0__.useMemo)(()=>({metadata:content.metadata,frontMatter:content.frontMatter,assets:content.assets,toc:content.toc,isBlogPostPage}),[content,isBlogPostPage]);}/**
 * This is a very thin layer around the `content` received from the MDX loader.
 * It provides metadata about the blog post to the children tree.
 */function BlogPostProvider(_ref2){let{children,content,isBlogPostPage=false}=_ref2;const contextValue=useContextValue({content,isBlogPostPage});return/*#__PURE__*/react__WEBPACK_IMPORTED_MODULE_0__.createElement(Context.Provider,{value:contextValue},children);}/**
 * Returns the data of the currently browsed blog post. Gives access to
 * front matter, metadata, TOC, etc.
 * When swizzling a low-level component (e.g. the "Edit this page" link)
 * and you need some extra metadata, you don't have to drill the props
 * all the way through the component tree: simply use this hook instead.
 */function useBlogPost(){const blogPost=(0,react__WEBPACK_IMPORTED_MODULE_0__.useContext)(Context);if(blogPost===null){throw new _utils_reactUtils__WEBPACK_IMPORTED_MODULE_1__/* .ReactContextError */ .i6('BlogPostProvider');}return blogPost;}

/***/ }),

/***/ 88824:
/***/ ((__unused_webpack_module, __webpack_exports__, __webpack_require__) => {

/* harmony export */ __webpack_require__.d(__webpack_exports__, {
/* harmony export */   "c": () => (/* binding */ usePluralForm)
/* harmony export */ });
/* harmony import */ var react__WEBPACK_IMPORTED_MODULE_0__ = __webpack_require__(67294);
/* harmony import */ var _docusaurus_useDocusaurusContext__WEBPACK_IMPORTED_MODULE_1__ = __webpack_require__(52263);
/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */// We want to ensurer a stable plural form order in all cases
// It is more convenient and natural to handle "small values" first
// See https://twitter.com/sebastienlorber/status/1366820663261077510
const OrderedPluralForms=['zero','one','two','few','many','other'];function sortPluralForms(pluralForms){return OrderedPluralForms.filter(pf=>pluralForms.includes(pf));}// Hardcoded english/fallback implementation
const EnglishPluralForms={locale:'en',pluralForms:sortPluralForms(['one','other']),select:count=>count===1?'one':'other'};function createLocalePluralForms(locale){const pluralRules=new Intl.PluralRules(locale);return{locale,pluralForms:sortPluralForms(pluralRules.resolvedOptions().pluralCategories),select:count=>pluralRules.select(count)};}/**
 * Poor man's `PluralSelector` implementation, using an English fallback. We
 * want a lightweight, future-proof and good-enough solution. We don't want a
 * perfect and heavy solution.
 *
 * Docusaurus classic theme has only 2 deeply nested labels requiring complex
 * plural rules. We don't want to use `Intl` + `PluralRules` polyfills + full
 * ICU syntax (react-intl) just for that.
 *
 * Notes:
 * - 2021: 92+% Browsers support `Intl.PluralRules`, and support will increase
 * in the future
 * - NodeJS >= 13 has full ICU support by default
 * - In case of "mismatch" between SSR and Browser ICU support, React keeps
 * working!
 */function useLocalePluralForms(){const{i18n:{currentLocale}}=(0,_docusaurus_useDocusaurusContext__WEBPACK_IMPORTED_MODULE_1__/* ["default"] */ .Z)();return (0,react__WEBPACK_IMPORTED_MODULE_0__.useMemo)(()=>{try{return createLocalePluralForms(currentLocale);}catch(err){console.error("Failed to use Intl.PluralRules for locale \""+currentLocale+"\".\nDocusaurus will fallback to the default (English) implementation.\nError: "+err.message+"\n");return EnglishPluralForms;}},[currentLocale]);}function selectPluralMessage(pluralMessages,count,localePluralForms){const separator='|';const parts=pluralMessages.split(separator);if(parts.length===1){return parts[0];}if(parts.length>localePluralForms.pluralForms.length){console.error("For locale="+localePluralForms.locale+", a maximum of "+localePluralForms.pluralForms.length+" plural forms are expected ("+localePluralForms.pluralForms.join(',')+"), but the message contains "+parts.length+": "+pluralMessages);}const pluralForm=localePluralForms.select(count);const pluralFormIndex=localePluralForms.pluralForms.indexOf(pluralForm);// In case of not enough plural form messages, we take the last one (other)
// instead of returning undefined
return parts[Math.min(pluralFormIndex,parts.length-1)];}/**
 * Reads the current locale and returns an interface very similar to
 * `Intl.PluralRules`.
 */function usePluralForm(){const localePluralForm=useLocalePluralForms();return{selectMessage:(count,pluralMessages)=>selectPluralMessage(pluralMessages,count,localePluralForm)};}

/***/ })

}]);