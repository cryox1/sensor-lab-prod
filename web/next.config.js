/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  env: {
    NEXT_PUBLIC_API_BASE: process.env.NEXT_PUBLIC_API_BASE || "",
    NEXT_PUBLIC_WS_URL: process.env.NEXT_PUBLIC_WS_URL || "",
    NEXT_PUBLIC_API_PORT: process.env.NEXT_PUBLIC_API_PORT || "",
    NEXT_PUBLIC_API_WRITE_TOKEN: process.env.NEXT_PUBLIC_API_WRITE_TOKEN || "",
  },
};
module.exports = nextConfig;
