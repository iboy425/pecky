import type { SVGProps } from "react";

type IconName =
  | "jar"
  | "person"
  | "plus"
  | "edit"
  | "chevron"
  | "volume"
  | "muted"
  | "upload"
  | "bluetooth"
  | "database"
  | "check"
  | "close"
  | "trash"
  | "download";

export function PeckyIcon({
  name,
  ...props
}: SVGProps<SVGSVGElement> & { name: IconName }) {
  const common = {
    fill: "none",
    stroke: "currentColor",
    strokeWidth: 1.8,
    strokeLinecap: "round" as const,
    strokeLinejoin: "round" as const,
  };

  return (
    <svg aria-hidden="true" viewBox="0 0 24 24" {...common} {...props}>
      {name === "jar" && (
        <>
          <path d="M9 3h6M8.5 5h7v2.3c0 .9.4 1.7 1.1 2.3 1.5 1.3 2.4 3.2 2.4 5.2V18a3 3 0 0 1-3 3H8a3 3 0 0 1-3-3v-3.2c0-2 .9-3.9 2.4-5.2.7-.6 1.1-1.4 1.1-2.3V5Z" />
          <path d="M8.5 7.5h7" />
        </>
      )}
      {name === "person" && (
        <>
          <circle cx="12" cy="7" r="3.2" />
          <path d="M5.7 21v-3.2a6.3 6.3 0 0 1 12.6 0V21" />
        </>
      )}
      {name === "plus" && <path d="M12 5v14M5 12h14" />}
      {name === "edit" && (
        <>
          <path d="m14.2 5.2 4.6 4.6L9 19.6l-5 1 1-5 9.2-10.4Z" />
          <path d="m12.7 6.9 4.4 4.4" />
        </>
      )}
      {name === "chevron" && <path d="m9 5 7 7-7 7" />}
      {name === "volume" && (
        <>
          <path d="M4 10v4h3l4 3V7l-4 3H4Z" />
          <path d="M15 9.2a4 4 0 0 1 0 5.6M17.6 6.8a7.2 7.2 0 0 1 0 10.4" />
        </>
      )}
      {name === "muted" && (
        <>
          <path d="M4 10v4h3l4 3V7l-4 3H4Z" />
          <path d="m15 10 5 5m0-5-5 5" />
        </>
      )}
      {name === "upload" && (
        <>
          <path d="M12 16V4m0 0L7.5 8.5M12 4l4.5 4.5" />
          <path d="M5 14v5h14v-5" />
        </>
      )}
      {name === "download" && (
        <>
          <path d="M12 4v12m0 0 4.5-4.5M12 16l-4.5-4.5" />
          <path d="M5 19h14" />
        </>
      )}
      {name === "bluetooth" && (
        <>
          <path d="m10 4 5 4-5 4V4Zm0 8 5 4-5 4v-8Z" />
          <path d="m5 7 10 9M5 17 15 8" />
        </>
      )}
      {name === "database" && (
        <>
          <ellipse cx="12" cy="5.5" rx="7" ry="3" />
          <path d="M5 5.5v6c0 1.7 3.1 3 7 3s7-1.3 7-3v-6" />
          <path d="M5 11.5v6c0 1.7 3.1 3 7 3s7-1.3 7-3v-6" />
        </>
      )}
      {name === "check" && <path d="m5 12 4.2 4.2L19 6.5" />}
      {name === "close" && <path d="m6 6 12 12M18 6 6 18" />}
      {name === "trash" && (
        <>
          <path d="M4 7h16M9 3h6l1 4H8l1-4Z" />
          <path d="m7 7 1 14h8l1-14M10 11v6M14 11v6" />
        </>
      )}
    </svg>
  );
}
