import type { Metadata, Viewport } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "Pecky 啄米｜把每一粒米攒成小愿望",
  description: "记录每次啄米，把共同的米罐余额变成看得见的愿望进度。",
  applicationName: "Pecky 啄米",
  manifest: "/manifest.webmanifest",
  formatDetection: { telephone: false },
  appleWebApp: {
    capable: true,
    statusBarStyle: "default",
    title: "Pecky 啄米",
  },
  icons: {
    icon: [
      { url: "/assets/icons/pecky-192.png", sizes: "192x192", type: "image/png" },
      { url: "/assets/icons/pecky-512.png", sizes: "512x512", type: "image/png" },
    ],
    apple: [{ url: "/assets/icons/pecky-180.png", sizes: "180x180", type: "image/png" }],
  },
  openGraph: {
    title: "Pecky 啄米",
    description: "慢慢攒，愿望会实现。",
    type: "website",
    locale: "zh_CN",
    images: [{ url: "/og.png", width: 1200, height: 630, alt: "Pecky 啄米与小愿望米罐" }],
  },
};

export const viewport: Viewport = {
  width: "device-width",
  initialScale: 1,
  viewportFit: "cover",
  themeColor: "#FFF0BE",
  colorScheme: "light",
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="zh-CN">
      <body>{children}</body>
    </html>
  );
}
