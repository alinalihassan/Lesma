import * as monaco from 'monaco-editor';
import { IAPIClient } from '@site/src/services/api';
import { LANGUAGE_LESMA } from './props';

let alreadyRegistered = false;

// Language register
interface ILang extends monaco.languages.ILanguageExtensionPoint {
	loader: () => Promise<ILangImpl>;
}
const lazyLanguageLoaders: { [languageId: string]: LazyLanguageLoader } = {};

class LazyLanguageLoader {
	public static getOrCreate(languageId: string): LazyLanguageLoader {
		if (!lazyLanguageLoaders[languageId]) {
			lazyLanguageLoaders[languageId] = new LazyLanguageLoader(languageId);
		}
		return lazyLanguageLoaders[languageId];
	}

	private readonly _languageId: string;
	private _loadingTriggered: boolean;
	private _lazyLoadPromise: Promise<ILangImpl>;
	private _lazyLoadPromiseResolve!: (value: ILangImpl) => void;
	private _lazyLoadPromiseReject!: (err: any) => void;

	constructor(languageId: string) {
		this._languageId = languageId;
		this._loadingTriggered = false;
		this._lazyLoadPromise = new Promise((resolve, reject) => {
			this._lazyLoadPromiseResolve = resolve;
			this._lazyLoadPromiseReject = reject;
		});
	}

	public load(): Promise<ILangImpl> {
		if (!this._loadingTriggered) {
			this._loadingTriggered = true;
			languageDefinitions[this._languageId].loader().then(
				(mod) => this._lazyLoadPromiseResolve(mod),
				(err) => this._lazyLoadPromiseReject(err)
			);
		}
		return this._lazyLoadPromise;
	}
}

interface ILangImpl {
	conf: monaco.languages.LanguageConfiguration;
	language: monaco.languages.IMonarchLanguage;
}

const languageDefinitions: { [languageId: string]: ILang } = {};

export function registerLanguage(def: ILang): void {
	const languageId = def.id;

	languageDefinitions[languageId] = def;
	monaco.languages.register(def);

	const lazyLanguageLoader = LazyLanguageLoader.getOrCreate(languageId);
	monaco.languages.registerTokensProviderFactory(languageId, {
		create: async (): Promise<monaco.languages.IMonarchLanguage> => {
			const mod = await lazyLanguageLoader.load();
			return mod.language;
		}
	});
	monaco.languages.onLanguage(languageId, async () => {
		const mod = await lazyLanguageLoader.load();
		monaco.languages.setLanguageConfiguration(languageId, mod.conf);
	});
}

export const registerLesmaLanguageProvider = (client: IAPIClient) => {
  if (alreadyRegistered) {
    console.warn('Lesma Language provider was already registered');
    return;
  }

  alreadyRegistered = true;

  registerLanguage({
    id: LANGUAGE_LESMA,
    extensions: ['.les'],
    aliases: ['Lesma', 'les'],
    loader: () => {
      return import('./lesma');
    }
  });
  // return monaco.languages.registerCompletionItemProvider(LANGUAGE_LESMA, new LesmaCompletionItemProvider(client));
};

// Import aliases
// type CompletionList = monaco.languages.CompletionList;
// type CompletionContext = monaco.languages.CompletionContext;
// type ITextModel = monaco.editor.ITextModel;
// type Position = monaco.Position;
// type CancellationToken = monaco.CancellationToken;

// Matches package (and method name)
// const COMPL_REGEXP = /([a-zA-Z0-9_]+)(\.([A-Za-z0-9_]+))?$/;
// const R_GROUP_PKG = 1;
// const R_GROUP_METHOD = 3;

// const parseExpression = (expr: string) => {
//   COMPL_REGEXP.lastIndex = 0; // Reset regex state
//   const m = COMPL_REGEXP.exec(expr);
//   if (!m) {
//     return null;
//   }

//   const varName = m[R_GROUP_PKG];
//   const propValue = m[R_GROUP_METHOD];

//   if (!propValue) {
//     return { value: varName };
//   }

//   return {
//     packageName: varName,
//     value: propValue
//   };
// };
// 
// class LesmaCompletionItemProvider implements monaco.languages.CompletionItemProvider {
//   constructor(private client: IAPIClient) { }

//   async provideCompletionItems(model: ITextModel, position: Position, context: CompletionContext, token: CancellationToken): Promise<CompletionList> {
//     const val = model.getValueInRange({
//       startLineNumber: position.lineNumber,
//       startColumn: 0,
//       endLineNumber: position.lineNumber,
//       endColumn: position.column,
//     }).trim();

//     const query = parseExpression(val);
//     if (!query) {
//       return Promise.resolve({ suggestions: [] });
//     }

//     let word = model.getWordUntilPosition(position);
//     let range = {
//       startLineNumber: position.lineNumber,
//       endLineNumber: position.lineNumber,
//       startColumn: word.startColumn,
//       endColumn: word.endColumn
//     };

//     // filter snippets by prefix.
//     // usually monaco does that but not always in right way
//     const relatedSnippets = snippets
//       .filter(s => s.label.toString().startsWith(query.value))
//       .map(s => ({ ...s, range }));

//     try {
//       const { suggestions } = await this.client.getSuggestions(query);
//       if (!suggestions) {
//         return {
//           suggestions: relatedSnippets
//         }
//       }

//       return {
//         suggestions: relatedSnippets.concat(suggestions.map(s => ({ ...s, range })))
//       }
//     } catch (err: any) {
//       console.error(`Failed to get code completion from server: ${err.message}`);
//       return { suggestions: relatedSnippets };
//     }
//   }
// }
