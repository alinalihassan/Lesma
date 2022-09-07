import * as monaco from 'monaco-editor';

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

export const registerLesmaLanguageProvider = () => {
    if (alreadyRegistered) {
        console.warn('Lesma Language provider was already registered');
        return;
    }

    alreadyRegistered = true;

    registerLanguage({
        id: 'lesma',
        extensions: ['.les'],
        aliases: ['Lesma', 'les'],
        loader: () => {
            return import('./lesma');
        }
    });
};