import DefaultTheme from 'vitepress/theme'
import type { Theme } from 'vitepress'
import { searchTokenize } from '../search-tokenizer'
import './custom.css'

function withClientTokenizer(data: any) {
  const search = data?.themeConfig?.search
  if (search?.provider !== 'local') return null
  if (search.options?.miniSearch?.options?.tokenize === searchTokenize) return null

  return {
    ...data,
    themeConfig: {
      ...data.themeConfig,
      search: {
        ...search,
        options: {
          ...search.options,
          miniSearch: {
            ...search.options?.miniSearch,
            options: {
              ...search.options?.miniSearch?.options,
              tokenize: searchTokenize,
            },
          },
        },
      },
    },
  }
}

export default {
  extends: DefaultTheme,
  enhanceApp({ siteData }) {
    const patched = withClientTokenizer(siteData.value)
    if (patched) {
      siteData.value = patched
    }
  },
} satisfies Theme
