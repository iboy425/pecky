import { copyFile, mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import sharp from "sharp";

const projectRoot = process.cwd();
const sourceRoot = path.resolve(projectRoot, "..");
const outputRoot = path.join(projectRoot, "public", "assets");

const sources = {
  openingVideo: path.join(
    sourceRoot,
    "animation 新素材",
    "小鸡啄米-啄米+叫声音轨版开屏peck页面.mp4",
  ),
  jar: path.join(sourceRoot, "pecky-design-system-v01", "assets", "jar-original.png"),
  identity: path.join(
    sourceRoot,
    "pecky-design-system-v01",
    "reference",
    "pecky-r3-approved.png",
  ),
  achievements: path.join(
    sourceRoot,
    "animation 新素材",
    "pecky-me-achievements-v01me页面成就设计",
    "assets",
  ),
  rewards: path.join(sourceRoot, "animation 新素材", "奖励图标合集"),
  approvedStocks: path.join(
    sourceRoot,
    "pecky-design-system-v01",
    "reference",
    "raw",
    "stocks-source-requires-mask.png",
  ),
};

const rewardFiles = [
  ["bag", "奖励图标1-包包.png"],
  ["electronics", "奖励图标2-电子产品.png"],
  ["ticket", "奖励图标3-门票.png"],
  ["jewelry", "奖励图标4-珠宝.png"],
  ["sports", "奖励图标5-运动.png"],
  ["toy", "奖励图标6-玩偶.png"],
  ["travel", "奖励图标7-旅行.png"],
  ["gold", "奖励图标8-黄金.png"],
  ["stocks", "奖励图标9-股票.png"],
];

async function ensureDirectories() {
  await Promise.all(
    ["media", "rewards", "achievements", "icons"].map((directory) =>
      mkdir(path.join(outputRoot, directory), { recursive: true }),
    ),
  );
}

async function prepareJarArt() {
  const jarScene = sharp(sources.jar)
    .extract({ left: 365, top: 255, width: 1280, height: 1005 })
    .linear([1.1201, 1.153, 1.0905], [0, 0, 0]);

  await jarScene
    .clone()
    .resize({ width: 820, withoutEnlargement: true })
    .webp({ quality: 88, effort: 5 })
    .toFile(path.join(outputRoot, "jar-scene.webp"));

  const avatarBuffer = await sharp(sources.jar)
    .extract({ left: 1060, top: 500, width: 585, height: 585 })
    .linear([1.1201, 1.153, 1.0905], [0, 0, 0])
    .resize(360, 360, { fit: "cover" })
    .webp({ quality: 90, effort: 5 })
    .toBuffer();

  await sharp(avatarBuffer).toFile(path.join(outputRoot, "pecky-avatar.webp"));
}

async function prepareAppIcons() {
  const identity = await sharp(sources.identity)
    .extract({ left: 220, top: 190, width: 850, height: 850 })
    .resize(740, 740, { fit: "contain", background: "#FFFFFF" })
    .png()
    .toBuffer();

  for (const size of [180, 192, 512]) {
    const inset = Math.round(size * 0.09);
    const artwork = await sharp(identity)
      .resize(size - inset * 2, size - inset * 2)
      .png()
      .toBuffer();
    await sharp({
      create: {
        width: size,
        height: size,
        channels: 4,
        background: "#FFFFFF",
      },
    })
      .composite([{ input: artwork, left: inset, top: inset }])
      .png()
      .toFile(path.join(outputRoot, "icons", `pecky-${size}.png`));
  }

  const maskableInset = Math.round(512 * 0.18);
  const maskableArtwork = await sharp(identity)
    .resize(512 - maskableInset * 2, 512 - maskableInset * 2)
    .png()
    .toBuffer();
  await sharp({
    create: { width: 512, height: 512, channels: 4, background: "#FFFFFF" },
  })
    .composite([
      { input: maskableArtwork, left: maskableInset, top: maskableInset },
    ])
    .png()
    .toFile(path.join(outputRoot, "icons", "pecky-maskable-512.png"));
}

async function removeNeutralBackground(input) {
  const { data, info } = await sharp(input)
    .ensureAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });

  for (let index = 0; index < data.length; index += info.channels) {
    const red = data[index];
    const green = data[index + 1];
    const blue = data[index + 2];
    const saturation = Math.max(red, green, blue) - Math.min(red, green, blue);
    const opacity = Math.max(0, Math.min(255, Math.round((saturation - 2) * 25.5)));
    data[index + 3] = Math.min(data[index + 3], opacity);
  }

  return sharp(data, {
    raw: {
      width: info.width,
      height: info.height,
      channels: info.channels,
    },
  })
    .png()
    .toBuffer();
}

async function prepareRewardIcons() {
  await Promise.all(
    rewardFiles.map(async ([name, filename]) => {
      if (name === "stocks") {
        const mask = Buffer.from(`
          <svg width="1254" height="1254" viewBox="0 0 1254 1254">
            <path fill="white" d="M373 201 C516 187 850 188 951 205 C1042 219 1062 271 1061 363 L1061 886 C1058 989 1022 1034 924 1043 C740 1056 393 1053 294 1034 C219 1020 202 967 199 888 L198 379 C196 284 228 226 310 209 Z"/>
          </svg>
        `);
        const maskPng = await sharp(mask).png().toBuffer();
        const maskedStock = await sharp(sources.approvedStocks)
          .ensureAlpha()
          .composite([{ input: maskPng, blend: "dest-in" }])
          .png()
          .toBuffer();
        await sharp(maskedStock)
          .trim({ background: { r: 0, g: 0, b: 0, alpha: 0 } })
          .resize(192, 192, {
            fit: "contain",
            background: { r: 0, g: 0, b: 0, alpha: 0 },
          })
          .webp({ quality: 90, alphaQuality: 100, effort: 5 })
          .toFile(path.join(outputRoot, "rewards", `${name}.webp`));
        return;
      }

      if (name === "jewelry") {
        const transparentJewelry = await removeNeutralBackground(
          path.join(sources.rewards, filename),
        );
        await sharp(transparentJewelry)
          .trim({ background: { r: 0, g: 0, b: 0, alpha: 0 } })
          .resize(192, 192, {
            fit: "contain",
            background: { r: 0, g: 0, b: 0, alpha: 0 },
          })
          .webp({ quality: 90, alphaQuality: 100, effort: 5 })
          .toFile(path.join(outputRoot, "rewards", `${name}.webp`));
        return;
      }

      await sharp(path.join(sources.rewards, filename))
        .trim({ background: { r: 0, g: 0, b: 0, alpha: 0 } })
        .resize(192, 192, {
          fit: "contain",
          background: { r: 0, g: 0, b: 0, alpha: 0 },
        })
        .webp({ quality: 90, alphaQuality: 100, effort: 5 })
        .toFile(path.join(outputRoot, "rewards", `${name}.webp`));
    }),
  );
}

async function prepareAchievementIcons() {
  const items = [
    {
      name: "first-grain",
      file: "first-grain-original.png",
      crop: { left: 326, top: 147, width: 623, height: 934 },
    },
    {
      name: "hundred-pecks",
      file: "hundred-pecks-original.png",
      crop: { left: 182, top: 283, width: 890, height: 681 },
    },
    {
      name: "milk-tea",
      file: "milktea-original.png",
      crop: { left: 275, top: 168, width: 703, height: 938 },
    },
  ];

  await Promise.all(
    items.map(async ({ name, file, crop }) => {
      await sharp(path.join(sources.achievements, file))
        .extract(crop)
        .resize(192, 192, { fit: "contain", background: "#FFFFFF" })
        .webp({ quality: 92, effort: 5 })
        .toFile(path.join(outputRoot, "achievements", `${name}.webp`));
    }),
  );

  await sharp(path.join(sources.achievements, "wish-achieved-ricejar.png"))
    .trim({ background: { r: 0, g: 0, b: 0, alpha: 0 } })
    .resize(192, 192, {
      fit: "contain",
      background: { r: 0, g: 0, b: 0, alpha: 0 },
    })
    .webp({ quality: 92, alphaQuality: 100, effort: 5 })
    .toFile(path.join(outputRoot, "achievements", "wish-achieved.webp"));
}

async function prepareVideo() {
  await copyFile(
    sources.openingVideo,
    path.join(outputRoot, "media", "pecky-opening.mp4"),
  );
}

async function main() {
  await ensureDirectories();
  await Promise.all([
    prepareJarArt(),
    prepareAppIcons(),
    prepareRewardIcons(),
    prepareAchievementIcons(),
    prepareVideo(),
  ]);

  await writeFile(
    path.join(outputRoot, "prepared-assets.json"),
    `${JSON.stringify(
      {
        generatedAt: new Date().toISOString(),
        sourcePolicy: "Approved originals remain read-only; these are web delivery derivatives.",
        rewards: rewardFiles.map(([name]) => name),
      },
      null,
      2,
    )}\n`,
  );
}

await main();
