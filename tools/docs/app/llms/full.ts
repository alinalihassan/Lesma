import { getLLMText, source } from '@/lib/source';

export async function clientLoader() {
  const scan = source.getPages().map(getLLMText);
  const scanned = await Promise.all(scan);

  return new Response(scanned.join('\n\n'));
}
