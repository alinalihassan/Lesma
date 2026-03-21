// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

// https://astro.build/config
export default defineConfig({
	site: 'https://lesma-lang.com',
	integrations: [
		starlight({
			title: 'Lesma',
			description: 'Documentation for the Lesma programming language, compiler, and tooling.',
			logo: {
				src: './src/assets/logo.svg',
				alt: 'Lesma',
			},
			social: [
				{ icon: 'github', label: 'GitHub', href: 'https://github.com/alinalihassan/Lesma' },
			],
			editLink: {
				baseUrl: 'https://github.com/alinalihassan/Lesma/edit/main/tools/docs/',
			},
			sidebar: [
				{
					label: 'Introduction',
					items: [
						{ label: 'What is Lesma', slug: 'introduction/what-is-lesma' },
						{ label: 'Getting Started', slug: 'getting-started' },
						{ label: 'Hello World', slug: 'hello-world' },
						{ label: 'Guessing Game', slug: 'guessing-game' },
					],
				},
				{
					label: 'Language',
					items: [
						{ label: 'Literals', slug: 'language/literals' },
						{ label: 'Variables', slug: 'language/variables' },
						{ label: 'Types', slug: 'language/types' },
						{ label: 'Functions', slug: 'language/functions' },
						{ label: 'Control Flow', slug: 'language/control-flow' },
						{ label: 'Comments', slug: 'language/comments' },
					],
				},
				{
					label: 'Modules',
					items: [{ label: 'Overview', slug: 'modules/overview' }],
				},
			],
		}),
	],
});
