import { DOCUMENT } from "@angular/common";
import { Injectable, inject } from "@angular/core";
import { BehaviorSubject } from "rxjs";
import { BrowserStorageService } from "../browser-storage/browser-storage.service";
import {
  defaultDarkTheme,
  defaultLightTheme,
  ThemeKey,
  themes,
} from "./theme-registry";

const LOCAL_STORAGE_KEY = "rats-search-theme";

@Injectable({ providedIn: "root" })
export class ThemeManager {
  private document = inject(DOCUMENT);
  private browserStorage = inject(BrowserStorageService);
  private _window = this.document.defaultView;
  private selectedThemeSubject = new BehaviorSubject<ThemeKey | undefined>(
    undefined,
  );
  public selectedTheme$ = this.selectedThemeSubject.asObservable();
  public selectedTheme?: ThemeKey;
  public themes = [themes.classic, themes.tundra];

  constructor() {
    this.setActiveTheme(this.getPreferredTheme());
    this.windowMatchMediaPrefersDark()?.addEventListener("change", () => {
      const storedTheme = this.getStoredTheme();
      if (!storedTheme) {
        this.setActiveTheme(this.getAutoTheme());
      }
    });
  }

  getPreferredTheme = (): ThemeKey => {
    return this.getStoredTheme() ?? this.getAutoTheme();
  };

  getStoredTheme = (): ThemeKey | undefined => {
    const value = this.browserStorage.get(LOCAL_STORAGE_KEY);
    return value && value in themes ? (value as ThemeKey) : undefined;
  };

  getAutoTheme = (): ThemeKey => {
    return defaultDarkTheme;
  };

  setTheme = (theme: string) => {
    this.setActiveTheme(theme);
    this.setStoredTheme(this.selectedTheme ?? "auto");
  };

  private setActiveTheme = (theme: string) => {
    if (theme === "auto" || !(theme in themes)) {
      theme = this.getAutoTheme();
      this.selectedTheme = undefined;
    } else {
      this.selectedTheme = theme as ThemeKey;
    }
    const industrial = theme === "tundra" ? "dark-industrial" : "light-industrial";
    this.document.documentElement.setAttribute("data-theme", industrial);
    this.document.documentElement.setAttribute("data-bitmagnet-theme", theme);
    this.selectedThemeSubject.next(this.selectedTheme);
  };

  private setStoredTheme = (theme: ThemeKey | "auto") => {
    if (theme === "auto") {
      this.browserStorage.remove(LOCAL_STORAGE_KEY);
    } else {
      this.browserStorage.set(LOCAL_STORAGE_KEY, theme);
    }
  };

  private windowMatchMediaPrefersDark(): MediaQueryList | undefined {
    return this._window && this._window.matchMedia
      ? this._window.matchMedia("(prefers-color-scheme: dark)")
      : undefined;
  }
}
