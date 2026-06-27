export const metadata = {
  title: "sensor-lab",
  description: "ESP8266 + DHT11 telemetry",
};

export default function RootLayout({ children }) {
  return (
    <html lang="en">
      <body
        style={{
          margin: 0,
          fontFamily:
            "system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif",
          background: "#0e1116",
          color: "#e6e6e6",
          minHeight: "100vh",
        }}
      >
        {children}
      </body>
    </html>
  );
}
