import * as generated from "../graphql/generated";

type ContentTypeInfo = {
  singular: string;
  plural: string;
  icon: string;
};

export const contentTypeMap: Record<
  generated.ContentType | "null",
  ContentTypeInfo
> = {
  video: {
    singular: "Video",
    plural: "Videos",
    icon: "movie",
  },
  audio: {
    singular: "Audio",
    plural: "Audio",
    icon: "music_note",
  },
  books: {
    singular: "Book",
    plural: "Books",
    icon: "auto_stories",
  },
  pictures: {
    singular: "Picture",
    plural: "Pictures",
    icon: "image",
  },
  games: {
    singular: "Game",
    plural: "Games",
    icon: "sports_esports",
  },
  archive: {
    singular: "Archive",
    plural: "Archives",
    icon: "inventory_2",
  },
  bad: {
    singular: "Filtered",
    plural: "Filtered",
    icon: "block",
  },
  movie: {
    singular: "Movie",
    plural: "Movies",
    icon: "movie",
  },
  tv_show: {
    singular: "TV Show",
    plural: "TV Shows",
    icon: "live_tv",
  },
  music: {
    singular: "Music",
    plural: "Music",
    icon: "music_note",
  },
  ebook: {
    singular: "E-Book",
    plural: "E-Books",
    icon: "auto_stories",
  },
  comic: {
    singular: "Comic",
    plural: "Comics",
    icon: "comic_bubble",
  },
  audiobook: {
    singular: "Audiobook",
    plural: "Audiobooks",
    icon: "mic",
  },
  software: {
    singular: "Software",
    plural: "Software",
    icon: "desktop_windows",
  },
  game: {
    singular: "Game",
    plural: "Games",
    icon: "sports_esports",
  },
  xxx: {
    singular: "XXX",
    plural: "XXX",
    icon: "18_up_rating",
  },
  null: {
    singular: "Unknown",
    plural: "Unknown",
    icon: "question_mark",
  },
};

export const contentTypeList = Object.entries(contentTypeMap)
  .filter(([key]) =>
    [
      "video",
      "audio",
      "books",
      "pictures",
      "software",
      "games",
      "archive",
      "null",
    ].includes(key),
  )
  .map(([key, info]) => ({
    key: key as keyof typeof contentTypeMap,
    ...info,
  }));

export const contentTypeInfo = (
  key?: string | null,
): ContentTypeInfo | undefined =>
  key ? contentTypeMap[key as generated.ContentType] : undefined;
