"use strict";
(self["webpackChunklesma_website"] = self["webpackChunklesma_website"] || []).push([[7998],{

/***/ 80108:
/***/ ((__unused_webpack_module, __webpack_exports__, __webpack_require__) => {

// ESM COMPAT FLAG
__webpack_require__.r(__webpack_exports__);

// EXPORTS
__webpack_require__.d(__webpack_exports__, {
  "default": () => (/* binding */ playground)
});

// EXTERNAL MODULE: ./node_modules/@babel/runtime/helpers/esm/extends.js
var esm_extends = __webpack_require__(87462);
// EXTERNAL MODULE: ./node_modules/react/index.js
var react = __webpack_require__(67294);
// EXTERNAL MODULE: ./node_modules/react-redux/es/index.js + 20 modules
var es = __webpack_require__(14416);
// EXTERNAL MODULE: ./src/store/index.ts + 12 modules
var store = __webpack_require__(32845);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/CommandBar/CommandBar.js + 20 modules
var CommandBar = __webpack_require__(98281);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Modal/Modal.js + 5 modules
var Modal = __webpack_require__(11754);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Button/IconButton/IconButton.js + 1 modules
var IconButton = __webpack_require__(13396);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Dropdown/Dropdown.js + 13 modules
var Dropdown = __webpack_require__(22615);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Checkbox/Checkbox.js + 2 modules
var Checkbox = __webpack_require__(72267);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Pivot/Pivot.js + 5 modules
var Pivot = __webpack_require__(17137);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Pivot/PivotItem.js
var PivotItem = __webpack_require__(60019);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/utilities/ThemeProvider/ThemeContext.js
var ThemeContext = __webpack_require__(42141);
;// CONCATENATED MODULE: ./src/components/utils/ThemeableComponent.tsx
class ThemeableComponent extends react.Component{get theme(){return this.context;}}ThemeableComponent.contextType=ThemeContext/* ThemeContext */.N;
// EXTERNAL MODULE: ./node_modules/@fluentui/style-utilities/lib/index.js + 17 modules
var lib = __webpack_require__(70849);
;// CONCATENATED MODULE: ./src/styles/modal.ts
const getIconButtonStyles=theme=>(0,lib/* mergeStyleSets */.ZC)({root:{color:theme==null?void 0:theme.palette.neutralPrimary,marginLeft:'auto',marginTop:'4px',marginRight:'2px'},rootHovered:{color:theme==null?void 0:theme.palette.neutralDark}});const getContentStyles=theme=>(0,lib/* mergeStyleSets */.ZC)({container:{display:'flex',flexFlow:'column nowrap',alignItems:'stretch',width:'80%',maxWidth:'480px'},header:[theme==null?void 0:theme.fonts.xLargePlus,{flex:'1 1 auto',borderTop:"4px solid "+(theme==null?void 0:theme.palette.themePrimary),color:theme==null?void 0:theme.palette.neutralPrimary,display:'flex',fontSize:lib/* FontSizes.xLarge */.TS.xLarge,alignItems:'center',fontWeight:lib/* FontWeights.semibold */.lq.semibold,padding:'12px 12px 14px 24px'}],body:{flex:'4 4 auto',padding:'0 24px 24px 24px',overflowY:'hidden',selectors:{p:{margin:'14px 0'},'p:first-child':{marginTop:0},'p:last-child':{marginBottom:0}}}});
;// CONCATENATED MODULE: ./src/components/settings/styles.tsx
const settingsSectionStyles=(0,lib/* mergeStyleSets */.ZC)({title:{fontSize:lib/* FontSizes.xLarge */.TS.xLarge},section:{marginBottom:'25px'}});const settingsPropStyles=(0,lib/* mergeStyleSets */.ZC)({title:{fontWeight:lib/* FontWeights.bold */.lq.bold},container:{marginTop:'10px'},block:{marginTop:'15px'},description:{marginTop:'5px'}});
;// CONCATENATED MODULE: ./src/components/settings/SettingsProperty.tsx
function SettingsProperty(props){if(props.description){return/*#__PURE__*/react.createElement("div",{className:settingsPropStyles.block},/*#__PURE__*/react.createElement("div",{className:settingsPropStyles.title},props.title),/*#__PURE__*/react.createElement("div",{className:settingsPropStyles.description},props.description),/*#__PURE__*/react.createElement("div",{className:settingsPropStyles.container},props.control));}return/*#__PURE__*/react.createElement("div",{className:settingsPropStyles.block},/*#__PURE__*/react.createElement("div",{className:settingsPropStyles.title},props.title),/*#__PURE__*/react.createElement("div",{className:settingsPropStyles.container},props.control));}
// EXTERNAL MODULE: ./src/services/config.ts
var config = __webpack_require__(97691);
// EXTERNAL MODULE: ./src/services/fonts.ts
var fonts = __webpack_require__(80304);
;// CONCATENATED MODULE: ./src/components/settings/SettingsModal.tsx
var _dec,_class;// import {MessageBar, MessageBarType} from '@fluentui/react/lib/MessageBar';
// import {Link} from '@fluentui/react/lib/Link';
const CURSOR_BLINK_STYLE_OPTS=[{key:'blink',text:'Blink (default)'},{key:'smooth',text:'Smooth'},{key:'phase',text:'Phase'},{key:'expand',text:'Expand'},{key:'solid',text:'Solid'}];const CURSOR_LINE_OPTS=[{key:'line',text:'Line (default)'},{key:'block',text:'Block'},{key:'underline',text:'Underline'},{key:'line-thin',text:'Line thin'},{key:'block-outline',text:'Block outline'},{key:'underline-thin',text:'Underline thin'}];const FONT_OPTS=[{key:fonts/* DEFAULT_FONT */.Uo,text:'System default'},...(0,fonts/* getAvailableFonts */.jS)().map(f=>({key:f.family,text:f.label}))];let SettingsModal=(_dec=(0,store/* Connect */.lj)(state=>({settings:state.settings,monaco:state.monaco})),_dec(_class=class SettingsModal extends ThemeableComponent{constructor(props){super(props);this.titleID='Settings';this.subtitleID='SettingsSubText';this.changes={};this.state={isOpen:props.isOpen,showWarning:props.settings.runtime!==config/* RuntimeType.LesmaPlayground */.vm.LesmaPlayground};}onClose(){this.props.onClose({...this.changes});this.changes={};}touchMonacoProperty(key,val){if(!this.changes.monaco){this.changes.monaco={};}this.changes.monaco[key]=val;}touchSettingsProperty(changes){if(!this.changes.settings){this.changes.settings=changes;return;}this.changes.settings={...this.changes.settings,...changes};}render(){var _this$props$monaco$fo,_this$props$monaco,_this$props$settings,_this$props$monaco2,_this$props$monaco3,_this$props$monaco4,_this$props$monaco5,_this$props$monaco6,_this$props$monaco7,_this$props$monaco8,_this$props$monaco9;const contentStyles=getContentStyles(this.theme);const iconButtonStyles=getIconButtonStyles(this.theme);// const { showGoTipMessage, showWarning } = this.state;
return/*#__PURE__*/react.createElement(Modal/* Modal */.u,{titleAriaId:this.titleID,subtitleAriaId:this.subtitleID,isOpen:this.props.isOpen,onDismiss:()=>this.onClose(),containerClassName:contentStyles.container},/*#__PURE__*/react.createElement("div",{className:contentStyles.header},/*#__PURE__*/react.createElement("span",{id:this.titleID},"Settings"),/*#__PURE__*/react.createElement(IconButton/* IconButton */.h,{iconProps:{iconName:'Cancel'},styles:iconButtonStyles,ariaLabel:"Close popup modal",onClick:()=>this.onClose()})),/*#__PURE__*/react.createElement("div",{id:this.subtitleID,className:contentStyles.body},/*#__PURE__*/react.createElement(Pivot/* Pivot */.o,{"aria-label":"Settings"},/*#__PURE__*/react.createElement(PivotItem/* PivotItem */.M,{headerText:"Editor"},/*#__PURE__*/react.createElement(SettingsProperty,{key:"fontFamily",title:"Font Family",description:"Controls editor font family",control:/*#__PURE__*/react.createElement(Dropdown/* Dropdown */.L,{options:FONT_OPTS,defaultSelectedKey:(_this$props$monaco$fo=(_this$props$monaco=this.props.monaco)==null?void 0:_this$props$monaco.fontFamily)!=null?_this$props$monaco$fo:fonts/* DEFAULT_FONT */.Uo,onChange:(_,num)=>{this.touchMonacoProperty('fontFamily',num==null?void 0:num.key);}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"autoDetectTheme",title:"Use System Theme",control:/*#__PURE__*/react.createElement(Checkbox/* Checkbox */.X,{label:"Follow system dark mode preference instead of manual toggle.",defaultChecked:(_this$props$settings=this.props.settings)==null?void 0:_this$props$settings.useSystemTheme,onChange:(_,val)=>{this.touchSettingsProperty({useSystemTheme:val});}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"fontLigatures",title:"Font Ligatures",control:/*#__PURE__*/react.createElement(Checkbox/* Checkbox */.X,{label:"Enable programming font ligatures in supported fonts",defaultChecked:(_this$props$monaco2=this.props.monaco)==null?void 0:_this$props$monaco2.fontLigatures,onChange:(_,val)=>{this.touchMonacoProperty('fontLigatures',val);}})})),/*#__PURE__*/react.createElement(PivotItem/* PivotItem */.M,{headerText:"Advanced"},/*#__PURE__*/react.createElement(SettingsProperty,{key:"cursorBlinking",title:"Cursor Blinking",description:"Set cursor animation style",control:/*#__PURE__*/react.createElement(Dropdown/* Dropdown */.L,{options:CURSOR_BLINK_STYLE_OPTS,defaultSelectedKey:(_this$props$monaco3=this.props.monaco)==null?void 0:_this$props$monaco3.cursorBlinking,onChange:(_,num)=>{this.touchMonacoProperty('cursorBlinking',num==null?void 0:num.key);}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"cursorStyle",title:"Cursor Style",description:"Set the cursor style",control:/*#__PURE__*/react.createElement(Dropdown/* Dropdown */.L,{options:CURSOR_LINE_OPTS,defaultSelectedKey:(_this$props$monaco4=this.props.monaco)==null?void 0:_this$props$monaco4.cursorStyle,onChange:(_,num)=>{this.touchMonacoProperty('cursorStyle',num==null?void 0:num.key);}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"selectOnLineNumbers",title:"Select On Line Numbers",control:/*#__PURE__*/react.createElement(Checkbox/* Checkbox */.X,{label:"Select corresponding line on line number click",defaultChecked:(_this$props$monaco5=this.props.monaco)==null?void 0:_this$props$monaco5.selectOnLineNumbers,onChange:(_,val)=>{this.touchMonacoProperty('cursorStyle',val);}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"minimap",title:"Mini Map",control:/*#__PURE__*/react.createElement(Checkbox/* Checkbox */.X,{label:"Enable mini map on side",defaultChecked:(_this$props$monaco6=this.props.monaco)==null?void 0:_this$props$monaco6.minimap,onChange:(_,val)=>{this.touchMonacoProperty('minimap',val);}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"contextMenu",title:"Context Menu",control:/*#__PURE__*/react.createElement(Checkbox/* Checkbox */.X,{label:"Enable editor context menu (on right click)",defaultChecked:(_this$props$monaco7=this.props.monaco)==null?void 0:_this$props$monaco7.contextMenu,onChange:(_,val)=>{this.touchMonacoProperty('contextMenu',val);}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"smoothScroll",title:"Smooth Scrolling",control:/*#__PURE__*/react.createElement(Checkbox/* Checkbox */.X,{label:"Enable that the editor animates scrolling to a position",defaultChecked:(_this$props$monaco8=this.props.monaco)==null?void 0:_this$props$monaco8.smoothScrolling,onChange:(_,val)=>{this.touchMonacoProperty('smoothScrolling',val);}})}),/*#__PURE__*/react.createElement(SettingsProperty,{key:"mouseWheelZoom",title:"Mouse Wheel Zoom",control:/*#__PURE__*/react.createElement(Checkbox/* Checkbox */.X,{label:"Zoom the font in the editor when using the mouse wheel in combination with holding Ctrl",defaultChecked:(_this$props$monaco9=this.props.monaco)==null?void 0:_this$props$monaco9.mouseWheelZoom,onChange:(_,val)=>{this.touchMonacoProperty('mouseWheelZoom',val);}})})))));}})||_class);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/utilities/ThemeProvider/useTheme.js
var useTheme = __webpack_require__(73445);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Link/Link.js + 3 modules
var Link = __webpack_require__(52627);
;// CONCATENATED MODULE: ./src/components/modals/AboutModal.tsx
const TITLE_ID='AboutTitle';const SUB_TITLE_ID='AboutSubtitle';const modalStyles=(0,lib/* mergeStyleSets */.ZC)({title:{fontWeight:lib/* FontWeights.light */.lq.light,fontSize:lib/* FontSizes.xxLargePlus */.TS.xxLargePlus,padding:'1em 2em 2em 2em'},footer:{display:'flex',flexDirection:'row',justifyContent:'space-between',alignItems:'center'}});function AboutModal(props){const theme=(0,useTheme/* useTheme */.F)();const contentStyles=getContentStyles(theme);const iconButtonStyles=getIconButtonStyles(theme);return/*#__PURE__*/react.createElement(Modal/* Modal */.u,{titleAriaId:TITLE_ID,subtitleAriaId:SUB_TITLE_ID,isOpen:props.isOpen,onDismiss:props.onClose},/*#__PURE__*/react.createElement("div",{className:contentStyles.header},/*#__PURE__*/react.createElement("span",{id:TITLE_ID},"About"),/*#__PURE__*/react.createElement(IconButton/* IconButton */.h,{iconProps:{iconName:'Cancel'},styles:iconButtonStyles,ariaLabel:"Close popup modal",onClick:props.onClose})),/*#__PURE__*/react.createElement("div",{id:SUB_TITLE_ID,className:contentStyles.body},/*#__PURE__*/react.createElement("div",{className:modalStyles.title},"Lesma Playground"),/*#__PURE__*/react.createElement("div",{className:modalStyles.footer},/*#__PURE__*/react.createElement(Link/* Link */.r,{href:config/* default.githubUrl */.ZP.githubUrl,target:"_blank"},"GitHub"),/*#__PURE__*/react.createElement("span",null,"Version: ",config/* default.appVersion */.ZP.appVersion))));}AboutModal.defaultProps={isOpen:false};
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/ContextualMenu/ContextualMenu.types.js
var ContextualMenu_types = __webpack_require__(78612);
// EXTERNAL MODULE: ./src/services/snippets.ts + 1 modules
var snippets = __webpack_require__(70300);
;// CONCATENATED MODULE: ./src/utils/headerutils.ts
const snippetIconTypeMapping={[snippets/* SnippetType.Test */.u.Test]:{iconName:'TestPlan'},[snippets/* SnippetType.Experimental */.u.Experimental]:{iconName:'ATPLogo'}};/**
 * Returns snippets list dropdown menu items
 * @param handler menu item click handler
 */const getSnippetsMenuItems=handler=>{let menuItems=[];Object.entries(snippets/* default */.Z).forEach((s,i)=>{// this can be done in with ".map().reduce()"
// but imho simple imperative method will look
// more readable in future
const[sectionName,items]=s;const section={key:i.toString(),text:sectionName,itemType:ContextualMenu_types/* ContextualMenuItemType.Header */.n.Header};const sectionItems=items.map((item,ii)=>{return{key:i+"-"+ii,text:item.label,iconProps:item.icon?{iconName:item.icon}:getSnippetIconProps(item.type),onClick:()=>handler(item)};});menuItems.push(...[section,...sectionItems]);});return menuItems;};const getSnippetIconProps=snippetType=>{if(!snippetType)return;if(!(snippetType in snippetIconTypeMapping))return;return snippetIconTypeMapping[snippetType];};
;// CONCATENATED MODULE: ./src/components/core/Header.tsx
var Header_dec,Header_class;// import SharePopup from 'components/utils/SharePopup';
/**
 * Uniquie class name for share button to use as popover target.
 */ // const BTN_SHARE_CLASSNAME = 'Header__btn--share';
let Header=(Header_dec=(0,store/* Connect */.lj)(_ref=>{let{settings,status,ui}=_ref;return{darkMode:settings.darkMode,loading:status==null?void 0:status.loading,hideThemeToggle:settings.useSystemTheme,snippetName:(ui==null?void 0:ui.shareCreated)&&(ui==null?void 0:ui.snippetId)};}),Header_dec(Header_class=class Header extends ThemeableComponent{constructor(props){super(props);this.snippetMenuItems=getSnippetsMenuItems(i=>this.onSnippetMenuItemClick(i));this.state={showSettings:false,showAbout:false,loading:false// showShareMessage: false
};}componentDidMount(){const fileElement=document.createElement('input');fileElement.type='file';fileElement.accept='.les';fileElement.addEventListener('change',()=>this.onItemSelect(),false);this.fileInput=fileElement;}onItemSelect(){var _this$fileInput,_this$fileInput$files;const file=(_this$fileInput=this.fileInput)==null?void 0:(_this$fileInput$files=_this$fileInput.files)==null?void 0:_this$fileInput$files.item(0);if(!file){return;}this.props.dispatch((0,store/* newImportFileDispatcher */.Oh)(file));}onSnippetMenuItemClick(item){const dispatcher=item.snippet?(0,store/* newSnippetLoadDispatcher */.p6)(item.snippet):(0,store/* newCodeImportDispatcher */.XZ)(item.label,item.text);this.props.dispatch(dispatcher);}get menuItems(){return[{key:'openFile',text:'Open',split:true,iconProps:{iconName:'OpenFile'},disabled:this.props.loading,onClick:()=>{var _this$fileInput2;return(_this$fileInput2=this.fileInput)==null?void 0:_this$fileInput2.click();},subMenuProps:{items:this.snippetMenuItems}},{key:'run',text:'Run',ariaLabel:'Run program (Ctrl+Enter)',title:'Run program (Ctrl+Enter)',iconProps:{iconName:'Play'},disabled:this.props.loading,onClick:()=>{this.props.dispatch(store/* runFileDispatcher */.Od);}},// {
//   key: 'share',
//   text: 'Share',
//   className: BTN_SHARE_CLASSNAME,
//   iconProps: { iconName: 'Share' },
//   disabled: this.props.loading,
//   onClick: () => {
//     this.setState({ showShareMessage: true });
//     this.props.dispatch(shareSnippetDispatcher);
//   }
// },
{key:'download',text:'Download',iconProps:{iconName:'Download'},disabled:this.props.loading,onClick:()=>{this.props.dispatch(store/* saveFileDispatcher */.F_);}},{key:'settings',text:'Settings',ariaLabel:'Settings',iconProps:{iconName:'Settings'},disabled:this.props.loading,onClick:()=>{this.setState({showSettings:true});}}];}get asideItems(){return[// {
//   key: 'format',
//   text: 'Format Code',
//   ariaLabel: 'Format Code (Ctrl+Shift+F)',
//   iconOnly: true,
//   disabled: this.props.loading,
//   iconProps: { iconName: 'Code' },
//   onClick: () => {
//     this.props.dispatch(formatFileDispatcher);
//   }
// },
{key:'toggleTheme',text:'Toggle Dark Mode',ariaLabel:'Toggle Dark Mode',iconOnly:true,hidden:this.props.hideThemeToggle,iconProps:{iconName:this.props.darkMode?'Brightness':'ClearNight'},onClick:()=>{this.props.dispatch(store/* dispatchToggleTheme */.MY);}}];}get overflowItems(){return[{key:'new-issue',text:'Submit Issue',ariaLabel:'Submit Issue',iconProps:{iconName:'Bug'},onClick:()=>(0,react.useEffect)(()=>{window.open(config/* default.issueUrl */.ZP.issueUrl,'_blank');})},{key:'donate',text:'Donate',ariaLabel:'Donate',iconProps:{iconName:'Heart'},onClick:()=>(0,react.useEffect)(()=>{window.open(config/* default.donateUrl */.ZP.donateUrl,'_blank');})},{key:'about',text:'About',ariaLabel:'About',iconProps:{iconName:'Info'},onClick:()=>{this.setState({showAbout:true});}}];}onSettingsClose(changes){if(changes.monaco){// Update monaco state if some of it's settings were changed
this.props.dispatch((0,store/* newMonacoParamsChangeDispatcher */.au)(changes.monaco));}if(changes.args){// Save runtime settings
const{runtime,autoFormat}=changes.args;this.props.dispatch((0,store/* newBuildParamsChangeDispatcher */.Mj)(runtime,autoFormat));}if(changes.settings){this.props.dispatch((0,store/* newSettingsChangeDispatcher */.B4)(changes.settings));}this.setState({showSettings:false});}render(){// const { showShareMessage } = this.state;
// const { snippetName } = this.props;
return/*#__PURE__*/react.createElement("header",{className:"header",style:{backgroundColor:this.theme.palette.white}},/*#__PURE__*/react.createElement(CommandBar/* CommandBar */.X,{className:"header__commandBar",items:this.menuItems,farItems:this.asideItems.filter(_ref2=>{let{hidden}=_ref2;return!hidden;}),overflowItems:this.overflowItems,ariaLabel:"CodeEditor menu"}),/*#__PURE__*/react.createElement(SettingsModal,{onClose:args=>this.onSettingsClose(args),isOpen:this.state.showSettings}),/*#__PURE__*/react.createElement(AboutModal,{onClose:()=>this.setState({showAbout:false}),isOpen:this.state.showAbout}));}})||Header_class);
// EXTERNAL MODULE: ./node_modules/react-monaco-editor/lib/index.js + 3 modules
var react_monaco_editor_lib = __webpack_require__(2302);
// EXTERNAL MODULE: ./node_modules/monaco-editor/esm/vs/editor/editor.main.js + 268 modules
var editor_main = __webpack_require__(40238);
;// CONCATENATED MODULE: ./src/components/editor/commands.ts
/**
 * MonacoDIContainer is undocumented DI service container of monaco editor.
 */ /**
 * createActionAlias returns an action that triggers other monaco action.
 * @param keybinding Hotkeys
 * @param action Action name
 */const createActionAlias=(keybinding,action)=>({keybinding,callback:ed=>{var _ed$getAction;return(_ed$getAction=ed.getAction(action))==null?void 0:_ed$getAction.run();}});/**
 * Builtin custom actions
 */const commands=[createActionAlias(editor_main/* KeyMod.Shift */.DC.Shift|editor_main/* KeyCode.Enter */.VD.Enter,'editor.action.insertLineAfter')];const attachCustomCommands=editorInstance=>{commands.forEach(_ref=>{let{keybinding,callback}=_ref;return editorInstance.addCommand(keybinding,di=>callback(editorInstance,di));});};
// EXTERNAL MODULE: ./src/components/editor/props.ts
var props = __webpack_require__(53556);
;// CONCATENATED MODULE: ./src/components/editor/CodeEditor.tsx
var CodeEditor_dec,CodeEditor_class;let CodeEditor=(CodeEditor_dec=(0,store/* Connect */.lj)(s=>{var _s$status;return{code:s.editor.code,darkMode:s.settings.darkMode,loading:(_s$status=s.status)==null?void 0:_s$status.loading,options:s.monaco};}),CodeEditor_dec(CodeEditor_class=class CodeEditor extends react.Component{editorDidMount(editorInstance,_){var _this=this;const actions=[{id:'clear',label:'Reset contents',contextMenuGroupId:'navigation',run:()=>{this.props.dispatch((0,store/* newSnippetLoadDispatcher */.p6)());}},{id:'run-code',label:'Run Code',contextMenuGroupId:'navigation',keybindings:[editor_main/* KeyMod.CtrlCmd */.DC.CtrlCmd|editor_main/* KeyCode.Enter */.VD.Enter],run:function(ed){_this.props.dispatch(store/* runFileDispatcher */.Od);}}// {
//   id: 'format-code',
//   label: 'Format Code',
//   contextMenuGroupId: 'navigation',
//   keybindings: [
//     monaco.KeyMod.CtrlCmd | monaco.KeyMod.Shift | monaco.KeyCode.KeyF
//   ],
//   run: (ed, ...args) => {
//     this.props.dispatch(formatFileDispatcher);
//   }
// }
];// Register custom actions
actions.forEach(action=>editorInstance.addAction(action));attachCustomCommands(editorInstance);editorInstance.focus();}onChange(newValue,_){this.props.dispatch((0,store/* newFileChangeAction */.bY)(newValue));}render(){const options=(0,props/* stateToOptions */.uO)(this.props.options);return/*#__PURE__*/react.createElement(react_monaco_editor_lib/* default */.ZP,{language:props/* LANGUAGE_LESMA */.yY,theme:this.props.darkMode?'vs-dark':'vs-light',value:this.props.code,options:options,onChange:(newVal,e)=>this.onChange(newVal,e),editorDidMount:(e,m)=>this.editorDidMount(e,m)});}})||CodeEditor_class);
;// CONCATENATED MODULE: ./src/components/editor/FlexContainer.tsx
const FlexContainer=_ref=>{let{children}=_ref;return/*#__PURE__*/react.createElement("div",{style:{background:'#000',flex:'1 1',overflow:'hidden'}},children);};/* harmony default export */ const editor_FlexContainer = (FlexContainer);
// EXTERNAL MODULE: ./node_modules/re-resizable/lib/index.js + 1 modules
var re_resizable_lib = __webpack_require__(46312);
// EXTERNAL MODULE: ./node_modules/clsx/dist/clsx.m.js
var clsx_m = __webpack_require__(86010);
// EXTERNAL MODULE: ./node_modules/react-icons/vsc/index.esm.js + 4 modules
var index_esm = __webpack_require__(17152);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/MessageBar/MessageBar.js + 3 modules
var MessageBar = __webpack_require__(76660);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/MessageBar/MessageBar.types.js
var MessageBar_types = __webpack_require__(94248);
;// CONCATENATED MODULE: ./src/components/preview/EvalEventView.tsx
const imageSectionPrefix='IMAGE:';const base64RegEx=/^[A-Za-z0-9+/]+[=]{0,2}$/;const isImageLine=message=>{if(!(message!=null&&message.startsWith(imageSectionPrefix))){return[false,null];}const payload=message.substring(imageSectionPrefix.length).trim();return[base64RegEx.test(payload),payload];};class EvalEventView extends react.Component{get delay(){const msec=this.props.delay;return"T+"+msec.toFixed(3)+"s";}get domClassName(){return"evalEvent__msg evalEvent__msg--"+this.props.kind;}render(){const{message,showDelay}=this.props;const[isImage,payload]=isImageLine(message);return/*#__PURE__*/react.createElement("div",{className:"evalEvent"},isImage?/*#__PURE__*/react.createElement("img",{src:"data:image;base64,"+payload,alt:""}):/*#__PURE__*/react.createElement("pre",{className:this.domClassName},message),/*#__PURE__*/react.createElement("span",{className:"evalEvent__delay"},showDelay?"["+this.delay+"]":''));}}
;// CONCATENATED MODULE: ./src/components/preview/Preview.tsx
var Preview_dec,Preview_class;let Preview=(Preview_dec=(0,store/* Connect */.lj)(s=>({darkMode:s.settings.darkMode,runtime:s.settings.runtime,...s.status})),Preview_dec(Preview_class=class Preview extends ThemeableComponent{get styles(){const{palette}=this.theme;return{backgroundColor:palette.neutralLight,color:palette.neutralDark,fontFamily:(0,fonts/* getDefaultFontFamily */.M1)()};}render(){let content;if(this.props.lastError){content=/*#__PURE__*/react.createElement(MessageBar/* MessageBar */.c,{messageBarType:MessageBar_types/* MessageBarType.error */.f.error,isMultiline:true},/*#__PURE__*/react.createElement("b",{className:"app-preview__label"},"Error"),/*#__PURE__*/react.createElement("pre",{className:"app-preview__errors"},this.props.lastError));}else if(this.props.events){content=this.props.events.map((_ref,k)=>{let{Message,Delay,Kind}=_ref;return/*#__PURE__*/react.createElement(EvalEventView,{key:k,message:Message,delay:Delay,kind:Kind,showDelay:true});});content.push(/*#__PURE__*/react.createElement("div",{className:"app-preview__epilogue",key:"exit"},"Program exited."));}else{content=/*#__PURE__*/react.createElement("span",null,"Press \"Run\" to compile program.");}return/*#__PURE__*/react.createElement("div",{className:"app-preview",style:this.styles},/*#__PURE__*/react.createElement("div",{className:"app-preview__content"},content));}})||Preview_class);
;// CONCATENATED MODULE: ./src/components/core/Panel/PanelAction.tsx
const PanelAction=_ref=>{let{hidden,icon,desktopOnly,label,onClick}=_ref;if(hidden){return null;}return/*#__PURE__*/react.createElement("button",{className:(0,clsx_m/* default */.Z)('PanelAction',desktopOnly&&'PanelAction--desktopOnly'),title:label,onClick:onClick},icon);};/* harmony default export */ const Panel_PanelAction = (PanelAction);
;// CONCATENATED MODULE: ./src/components/core/Panel/PanelHeader.tsx
const PanelHeader=_ref=>{let{label,commands}=_ref;const theme=(0,react.useContext)(ThemeContext/* ThemeContext */.N);const{palette:{neutralLight,neutralDark,neutralQuaternaryAlt}}=theme;return/*#__PURE__*/react.createElement("div",{className:"PanelHeader",style:{backgroundColor:neutralLight,color:neutralDark,'--pg-panel-action-hover-bg':neutralQuaternaryAlt}},/*#__PURE__*/react.createElement("div",{className:"PanelHeader__side--left"},/*#__PURE__*/react.createElement("span",{className:"PanelHeader__title"},label)),/*#__PURE__*/react.createElement("ul",{className:"PanelHeader__commands"},commands?Object.entries(commands).map(_ref2=>{let[key,props]=_ref2;return{key,...props};}).filter(_ref3=>{let{hidden}=_ref3;return!hidden;}).map(_ref4=>{let{key,...props}=_ref4;return/*#__PURE__*/react.createElement("li",{key:key},/*#__PURE__*/react.createElement(Panel_PanelAction,props));}):null));};/* harmony default export */ const Panel_PanelHeader = (PanelHeader);
// EXTERNAL MODULE: ./src/styles/layout.ts
var styles_layout = __webpack_require__(49145);
;// CONCATENATED MODULE: ./src/components/preview/ResizablePreview.tsx
const MIN_HEIGHT=36;const MIN_WIDTH=120;const handleClasses={top:'ResizablePreview__handle--top',left:'ResizablePreview__handle--left'};const ResizablePreview=_ref=>{let{layout=styles_layout/* LayoutType.Vertical */.ms.Vertical,height=styles_layout/* DEFAULT_PANEL_HEIGHT */.Uo,width=styles_layout/* DEFAULT_PANEL_WIDTH */.wE,collapsed,onViewChange}=_ref;const{palette:{accent},semanticColors:{buttonBorder}}=(0,useTheme/* useTheme */.F)();const onResize=(0,react.useCallback)((e,direction,ref,size)=>{switch(layout){case styles_layout/* LayoutType.Vertical */.ms.Vertical:onViewChange==null?void 0:onViewChange({height:height+size.height});return;case styles_layout/* LayoutType.Horizontal */.ms.Horizontal:onViewChange==null?void 0:onViewChange({width:width+size.width});return;default:return;}},[height,width,layout,onViewChange]);const size={height:layout===styles_layout/* LayoutType.Vertical */.ms.Vertical?height:'100%',width:layout===styles_layout/* LayoutType.Horizontal */.ms.Horizontal?width:'100%'};const enabledCorners={top:!collapsed&&layout===styles_layout/* LayoutType.Vertical */.ms.Vertical,right:false,bottom:false,left:!collapsed&&layout===styles_layout/* LayoutType.Horizontal */.ms.Horizontal,topRight:false,bottomRight:false,bottomLeft:false,topLeft:false};const isCollapsed=collapsed&&layout===styles_layout/* LayoutType.Vertical */.ms.Vertical;return/*#__PURE__*/react.createElement(re_resizable_lib/* Resizable */.e,{className:(0,clsx_m/* default */.Z)('ResizablePreview',isCollapsed&&'ResizablePreview--collapsed',"ResizablePreview--"+layout),handleClasses:handleClasses,size:size,enable:enabledCorners,onResizeStop:onResize,minHeight:MIN_HEIGHT,minWidth:MIN_WIDTH,style:{'--pg-handle-active-color':accent,'--pg-handle-default-color':buttonBorder}},/*#__PURE__*/react.createElement(Panel_PanelHeader,{label:"Output",commands:{'vertical-layout':{hidden:layout===styles_layout/* LayoutType.Vertical */.ms.Vertical,icon:/*#__PURE__*/react.createElement(index_esm/* VscSplitVertical */.Gaf,null),label:'Use vertical layout',onClick:()=>onViewChange==null?void 0:onViewChange({layout:styles_layout/* LayoutType.Vertical */.ms.Vertical})},'horizontal-layout':{desktopOnly:true,hidden:layout===styles_layout/* LayoutType.Horizontal */.ms.Horizontal,icon:/*#__PURE__*/react.createElement(index_esm/* VscSplitHorizontal */.KH2,null),label:'Use horizontal layout',onClick:()=>onViewChange==null?void 0:onViewChange({layout:styles_layout/* LayoutType.Horizontal */.ms.Horizontal})},'collapse':{hidden:layout===styles_layout/* LayoutType.Horizontal */.ms.Horizontal,icon:collapsed?/*#__PURE__*/react.createElement(index_esm/* VscChevronUp */.L2V,null):/*#__PURE__*/react.createElement(index_esm/* VscChevronDown */.qhg,null),label:collapsed?'Expand':'Collapse',onClick:()=>onViewChange==null?void 0:onViewChange({collapsed:!collapsed})}}}),isCollapsed?null:/*#__PURE__*/react.createElement(Preview,null));};/* harmony default export */ const preview_ResizablePreview = (ResizablePreview);
;// CONCATENATED MODULE: ./src/components/core/Layout/Layout.tsx
const Layout=_ref=>{let{layout=styles_layout/* LayoutType.Vertical */.ms.Vertical,children}=_ref;return/*#__PURE__*/react.createElement("div",{className:"Layout Layout--"+layout},children);};/* harmony default export */ const Layout_Layout = (Layout);
// EXTERNAL MODULE: ./node_modules/@docusaurus/theme-classic/lib/theme/Layout/index.js + 69 modules
var theme_Layout = __webpack_require__(67767);
;// CONCATENATED MODULE: ./src/components/utils/EllipsisText.tsx
const EllipsisText=_ref=>{let{children,...props}=_ref;return/*#__PURE__*/react.createElement("span",(0,esm_extends/* default */.Z)({className:"EllipsisText"},props),children);};/* harmony default export */ const utils_EllipsisText = (EllipsisText);
// EXTERNAL MODULE: ./node_modules/@fluentui/react/lib/components/Icon/FontIcon.js
var FontIcon = __webpack_require__(20316);
;// CONCATENATED MODULE: ./src/components/core/StatusBar/StatusBarItem.tsx
const getIcon=icon=>typeof icon==='string'?/*#__PURE__*/react.createElement(FontIcon/* FontIcon */.xu,{iconName:icon,className:"StatusBarItem__icon"}):/*#__PURE__*/react.createElement(icon,{className:'StatusBarItem__icon'});const getItemContents=_ref=>{let{icon,iconOnly,imageSrc,title,children}=_ref;return/*#__PURE__*/react.createElement(react.Fragment,null,icon&&getIcon(icon),imageSrc&&/*#__PURE__*/react.createElement("img",{src:imageSrc,className:"StatusBarItem__icon--image",alt:title}),!iconOnly&&/*#__PURE__*/react.createElement("span",{className:"StatusBarItem__label"},children));};const StatusBarItem=_ref2=>{let{title,className,icon,iconOnly,imageSrc,hideTextOnMobile,href,button,children,hidden,...props}=_ref2;if(hidden){return null;}const content=getItemContents({icon,iconOnly,children,imageSrc,title});const classValue=hideTextOnMobile?'StatusBarItem StatusBarItem--hideOnMobile':'StatusBarItem';if(button){return/*#__PURE__*/react.createElement("button",(0,esm_extends/* default */.Z)({className:(0,clsx_m/* default */.Z)(classValue+" StatusBarItem--action",className),title:title},props),content);}if(href){return/*#__PURE__*/react.createElement("a",(0,esm_extends/* default */.Z)({href:href,target:"_blank",rel:"noreferrer",className:(0,clsx_m/* default */.Z)(classValue+" StatusBarItem--action",className),title:title},props),content);}const{style}=props;return/*#__PURE__*/react.createElement("div",{className:(0,clsx_m/* default */.Z)(classValue+" StatusBarItem--text",className),title:title,style:style},content);};/* harmony default export */ const StatusBar_StatusBarItem = (StatusBarItem);
;// CONCATENATED MODULE: ./src/components/core/StatusBar/StatusBar.tsx
// import {newEnvironmentChangeAction} from 'store';
const getStatusItem=_ref=>{let{loading,lastError}=_ref;if(loading){return/*#__PURE__*/react.createElement(StatusBar_StatusBarItem,{icon:"Build"},/*#__PURE__*/react.createElement(utils_EllipsisText,null,"Loading"));}if(lastError){return/*#__PURE__*/react.createElement(StatusBar_StatusBarItem,{icon:"NotExecuted",hideTextOnMobile:true,disabled:true},"Build failed");}return null;};const StatusBar=_ref2=>{var _markers$length;let{loading,lastError,runtime,markers,dispatch}=_ref2;// const [ runSelectorModalVisible, setRunSelectorModalVisible ] = useState(false);
const className=loading?'StatusBar StatusBar--busy':'StatusBar';return/*#__PURE__*/react.createElement(react.Fragment,null,/*#__PURE__*/react.createElement("div",{className:className},/*#__PURE__*/react.createElement("div",{className:"StatusBar__side-left"},/*#__PURE__*/react.createElement(StatusBar_StatusBarItem,{icon:"ErrorBadge",title:"No Problems",button:true},(_markers$length=markers==null?void 0:markers.length)!=null?_markers$length:0," Errors"),getStatusItem({loading,lastError})),/*#__PURE__*/react.createElement("div",{className:"StatusBar__side-right"},/*#__PURE__*/react.createElement(StatusBar_StatusBarItem,{icon:"Feedback",title:"Send feedback",href:config/* default.issueUrl */.ZP.issueUrl,iconOnly:true}),/*#__PURE__*/react.createElement(StatusBar_StatusBarItem,{imageSrc:"/github-mark-light-32px.png",title:"GitHub",href:config/* default.githubUrl */.ZP.githubUrl,iconOnly:true}))));};/* harmony default export */ const StatusBar_StatusBar = ((0,es/* connect */.$j)(_ref3=>{let{status:{loading,lastError,markers},settings:{runtime}}=_ref3;return{loading,lastError,markers,runtime};})(StatusBar));
;// CONCATENATED MODULE: ./src/components/core/StatusBar/index.ts
/* harmony default export */ const core_StatusBar = (StatusBar_StatusBar);
// EXTERNAL MODULE: ./node_modules/@docusaurus/core/lib/client/exports/useDocusaurusContext.js
var useDocusaurusContext = __webpack_require__(52263);
;// CONCATENATED MODULE: ./src/pages/playground/styles.module.css
// extracted by mini-css-extract-plugin
/* harmony default export */ const styles_module = ({"Playground":"Playground_xR_K"});
;// CONCATENATED MODULE: ./src/pages/playground/index.tsx
const CodeContainer=(0,es/* connect */.$j)()(_ref=>{let{dispatch}=_ref;// const { snippetID } = useParams();
dispatch((0,store/* newSnippetLoadDispatcher */.p6)());return/*#__PURE__*/react.createElement(CodeEditor,null);});const IDE=(0,es/* connect */.$j)(_ref2=>{let{panel}=_ref2;return{panelProps:panel};})(_ref3=>{let{panelProps,dispatch}=_ref3;return/*#__PURE__*/react.createElement("div",{className:"container"},/*#__PURE__*/react.createElement("div",{className:styles_module.Playground},/*#__PURE__*/react.createElement(Header,null),/*#__PURE__*/react.createElement(Layout_Layout,{layout:panelProps.layout},/*#__PURE__*/react.createElement(editor_FlexContainer,null,/*#__PURE__*/react.createElement(CodeContainer,null)),/*#__PURE__*/react.createElement(preview_ResizablePreview,(0,esm_extends/* default */.Z)({},panelProps,{onViewChange:changes=>dispatch((0,store/* dispatchPanelLayoutChange */.Wf)(changes))}))),/*#__PURE__*/react.createElement(core_StatusBar,null)));});const Playground=(0,es/* connect */.$j)(_ref4=>{let{panel}=_ref4;return{panelProps:panel};})(_ref5=>{let{panelProps,dispatch}=_ref5;const{siteConfig}=(0,useDocusaurusContext/* default */.Z)();return/*#__PURE__*/react.createElement(theme_Layout/* default */.Z,{title:"Lesma Programming Language",description:"Lesma Playground",noFooter:true},/*#__PURE__*/react.createElement(IDE,null));});/* harmony default export */ const playground = (Playground);

/***/ })

}]);