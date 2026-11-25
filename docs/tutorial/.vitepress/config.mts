import { defineConfig } from 'vitepress'
import { withMermaid } from 'vitepress-plugin-mermaid'
import { searchTokenize } from './search-tokenizer'

const base = process.env.VITEPRESS_BASE || '/'

export default withMermaid(
  defineConfig({
    base,
    cleanUrls: true,
    lastUpdated: false,

    head: [
      ['link', { rel: 'icon', type: 'image/png', href: `${base}obs-logo.png` }],
      ['meta', { name: 'theme-color', content: '#2563eb' }],
    ],

    locales: {
      root: {
        label: '简体中文',
        lang: 'zh-CN',
        title: 'OBS for Android',
        description: 'Android 平台直播推流与本地录制工具使用文档',
        themeConfig: {
          nav: [
            {
              text: '快速入门',
              link: '/quick-start/download-and-install',
              activeMatch: '^/quick-start/',
            },
            {
              text: '功能指南',
              activeMatch: '^/(scenes-and-sources|streaming-and-recording|advanced|compatibility)/',
              items: [
                { text: '场景与来源', link: '/scenes-and-sources/scene-management' },
                { text: '直播与录制', link: '/streaming-and-recording/rtmp-and-srt' },
                { text: '高级功能', link: '/advanced/hdr-and-colorspace' },
                { text: '设备兼容性', link: '/compatibility/soc-and-encoders' },
              ],
            },
            {
              text: '故障排查',
              link: '/troubleshooting/capture-black-screen',
              activeMatch: '^/troubleshooting/',
            },
            {
              text: '项目',
              activeMatch: '^/(README|project/)',
              items: [
                { text: '文档总目录', link: '/README' },
                { text: '更新日志', link: '/project/changelog' },
                { text: '参与贡献', link: '/project/contributing' },
                { text: '开源许可证', link: '/project/license' },
                { text: 'GitHub Releases', link: 'https://github.com/Keee7/OBS-for-Android/releases' },
              ],
            },
          ],

          sidebar: [
            {
              text: '快速入门',
              collapsed: false,
              items: [
                { text: '下载安装', link: '/quick-start/download-and-install' },
                { text: '系统与设备要求', link: '/quick-start/system-requirements' },
                { text: '界面导览与触控操作', link: '/quick-start/interface-overview' },
                { text: '第一次直播', link: '/quick-start/first-stream' },
              ],
            },
            {
              text: '场景与来源',
              collapsed: false,
              items: [
                { text: '场景管理', link: '/scenes-and-sources/scene-management' },
                { text: '摄像头与屏幕采集', link: '/scenes-and-sources/camera-and-screen' },
                { text: 'USB/UVC 采集卡', link: '/scenes-and-sources/usb-uvc-capture' },
                { text: '安卓浏览器与弹幕叠加', link: '/scenes-and-sources/browser-overlay' },
                { text: '网络媒体源', link: '/scenes-and-sources/network-media-source' },
              ],
            },
            {
              text: '直播与录制',
              collapsed: false,
              items: [
                { text: 'RTMP / SRT', link: '/streaming-and-recording/rtmp-and-srt' },
                { text: '视频编码器', link: '/streaming-and-recording/video-encoders' },
                { text: '音频设置', link: '/streaming-and-recording/audio-settings' },
                { text: '本地录制', link: '/streaming-and-recording/local-recording' },
                { text: '后台保活与防杀指南', link: '/streaming-and-recording/background-keepalive' },
              ],
            },
            {
              text: '高级功能',
              collapsed: false,
              items: [
                { text: 'HDR 与色彩空间', link: '/advanced/hdr-and-colorspace' },
                { text: '硬件编码', link: '/advanced/hardware-encoding' },
                { text: '性能与功耗优化', link: '/advanced/performance-and-power' },
                { text: 'NDI® 局域网收发与场景配方', link: '/advanced/ndi-and-recipes' },
              ],
            },
            {
              text: '兼容性',
              collapsed: false,
              items: [
                { text: 'SoC 与编码器支持', link: '/compatibility/soc-and-encoders' },
                { text: '采集卡兼容性', link: '/compatibility/capture-card-compatibility' },
                { text: 'Android 版本差异', link: '/compatibility/android-versions' },
              ],
            },
            {
              text: '故障排查',
              collapsed: false,
              items: [
                { text: '采集黑屏', link: '/troubleshooting/capture-black-screen' },
                { text: '直播卡顿', link: '/troubleshooting/stream-stuttering' },
                { text: '编码器报错', link: '/troubleshooting/encoder-errors' },
                { text: '日志收集与问题反馈', link: '/troubleshooting/logs-and-feedback' },
              ],
            },
            {
              text: '项目相关',
              collapsed: false,
              items: [
                { text: '更新日志', link: '/project/changelog' },
                { text: '参与贡献', link: '/project/contributing' },
                { text: '开源许可证', link: '/project/license' },
              ],
            },
          ],

          outline: {
            level: [2, 3],
            label: '本页目录',
          },

          docFooter: {
            prev: '上一页',
            next: '下一页',
          },

          darkModeSwitchLabel: '外观模式',
          lightModeSwitchTitle: '切换到浅色模式',
          darkModeSwitchTitle: '切换到深色模式',
          sidebarMenuLabel: '目录',
          returnToTopLabel: '回到顶部',
          langMenuLabel: '切换语言',

          footer: {
            message: '基于 GPL-2.0-or-later 开源 · 社区维护的 OBS Studio (v33) Android 移植版本',
            copyright: 'OBS for Android Documentation',
          },
        },
      },

      en: {
        label: 'English',
        lang: 'en-US',
        link: '/en/',
        title: 'OBS for Android',
        description: 'Live streaming and local recording on Android phones and tablets',
        themeConfig: {
          nav: [
            {
              text: 'Quick Start',
              link: '/en/quick-start/download-and-install',
              activeMatch: '^/en/quick-start/',
            },
            {
              text: 'Guides',
              activeMatch: '^/en/(scenes-and-sources|streaming-and-recording|advanced|compatibility)/',
              items: [
                { text: 'Scenes & Sources', link: '/en/scenes-and-sources/scene-management' },
                { text: 'Streaming & Recording', link: '/en/streaming-and-recording/rtmp-and-srt' },
                { text: 'Advanced', link: '/en/advanced/hdr-and-colorspace' },
                { text: 'Compatibility', link: '/en/compatibility/soc-and-encoders' },
              ],
            },
            {
              text: 'Troubleshooting',
              link: '/en/troubleshooting/capture-black-screen',
              activeMatch: '^/en/troubleshooting/',
            },
            {
              text: 'Project',
              activeMatch: '^/en/(README|project/)',
              items: [
                { text: 'Full Index', link: '/en/README' },
                { text: 'Changelog', link: '/en/project/changelog' },
                { text: 'Contributing', link: '/en/project/contributing' },
                { text: 'Licenses & Notices', link: '/en/project/license' },
                { text: 'GitHub Releases', link: 'https://github.com/Keee7/OBS-for-Android/releases' },
              ],
            },
          ],

          sidebar: [
            {
              text: 'Quick Start',
              collapsed: false,
              items: [
                { text: 'Download and Install', link: '/en/quick-start/download-and-install' },
                { text: 'System & Device Requirements', link: '/en/quick-start/system-requirements' },
                { text: 'Interface Overview & Touch', link: '/en/quick-start/interface-overview' },
                { text: 'Your First Stream', link: '/en/quick-start/first-stream' },
              ],
            },
            {
              text: 'Scenes & Sources',
              collapsed: false,
              items: [
                { text: 'Scene Management', link: '/en/scenes-and-sources/scene-management' },
                { text: 'Camera & Screen Capture', link: '/en/scenes-and-sources/camera-and-screen' },
                { text: 'USB/UVC Capture Cards', link: '/en/scenes-and-sources/usb-uvc-capture' },
                { text: 'Android Browser & Overlays', link: '/en/scenes-and-sources/browser-overlay' },
                { text: 'Network Media Sources', link: '/en/scenes-and-sources/network-media-source' },
              ],
            },
            {
              text: 'Streaming & Recording',
              collapsed: false,
              items: [
                { text: 'RTMP / SRT', link: '/en/streaming-and-recording/rtmp-and-srt' },
                { text: 'Video Encoders', link: '/en/streaming-and-recording/video-encoders' },
                { text: 'Audio Settings', link: '/en/streaming-and-recording/audio-settings' },
                { text: 'Local Recording', link: '/en/streaming-and-recording/local-recording' },
                { text: 'Background Keep-Alive', link: '/en/streaming-and-recording/background-keepalive' },
              ],
            },
            {
              text: 'Advanced',
              collapsed: false,
              items: [
                { text: 'HDR & Color Spaces', link: '/en/advanced/hdr-and-colorspace' },
                { text: 'Hardware Encoding', link: '/en/advanced/hardware-encoding' },
                { text: 'Performance & Power', link: '/en/advanced/performance-and-power' },
                { text: 'NDI® LAN I/O & Recipes', link: '/en/advanced/ndi-and-recipes' },
              ],
            },
            {
              text: 'Compatibility',
              collapsed: false,
              items: [
                { text: 'SoC & Encoder Support', link: '/en/compatibility/soc-and-encoders' },
                { text: 'Capture Card Compatibility', link: '/en/compatibility/capture-card-compatibility' },
                { text: 'Android Version Differences', link: '/en/compatibility/android-versions' },
              ],
            },
            {
              text: 'Troubleshooting',
              collapsed: false,
              items: [
                { text: 'Black Screen Capture', link: '/en/troubleshooting/capture-black-screen' },
                { text: 'Stream Stuttering', link: '/en/troubleshooting/stream-stuttering' },
                { text: 'Encoder Errors', link: '/en/troubleshooting/encoder-errors' },
                { text: 'Logs & Bug Reporting', link: '/en/troubleshooting/logs-and-feedback' },
              ],
            },
            {
              text: 'Project',
              collapsed: false,
              items: [
                { text: 'Changelog', link: '/en/project/changelog' },
                { text: 'Contributing', link: '/en/project/contributing' },
                { text: 'Licenses & Notices', link: '/en/project/license' },
              ],
            },
          ],

          outline: {
            level: [2, 3],
            label: 'On this page',
          },

          docFooter: {
            prev: 'Previous page',
            next: 'Next page',
          },

          footer: {
            message: 'Released under GPL-2.0-or-later · Community port of OBS Studio (v33) for Android',
            copyright: 'OBS for Android Documentation',
          },
        },
      },
    },

    themeConfig: {
      logo: '/obs-logo.png',
      siteTitle: 'OBS for Android',

      search: {
        provider: 'local',
        options: {
          miniSearch: {
            options: {
              tokenize: searchTokenize,
            },
          },
          locales: {
            root: {
              translations: {
                button: {
                  buttonText: '搜索文档',
                  buttonAriaLabel: '搜索文档',
                },
                modal: {
                  noResultsText: '没有找到相关内容',
                  resetButtonTitle: '清除搜索条件',
                  footer: {
                    selectText: '选择',
                    navigateText: '切换',
                    closeText: '关闭',
                  },
                },
              },
            },
            en: {
              translations: {
                button: {
                  buttonText: 'Search',
                  buttonAriaLabel: 'Search documentation',
                },
                modal: {
                  noResultsText: 'No matching results found',
                  resetButtonTitle: 'Reset query',
                  footer: {
                    selectText: 'to select',
                    navigateText: 'to navigate',
                    closeText: 'to close',
                  },
                },
              },
            },
          },
        },
      },

      socialLinks: [
        { icon: 'github', link: 'https://github.com/Keee7/OBS-for-Android' },
      ],
    },

    mermaid: {
      theme: 'neutral',
    },
  }),
)
