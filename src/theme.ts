import { useEffect, useState } from "react";

export const SIMULATOR_THEME_STORAGE_KEY = "panda-simulator-theme";
export const SIMULATOR_THEME_MEDIA_QUERY = "(prefers-color-scheme: dark)";

export type ThemePreference = "light" | "dark" | "system";
export type ResolvedTheme = "light" | "dark";

const DEFAULT_THEME_PREFERENCE: ThemePreference = "system";

export const parseThemePreference = (value: string | null | undefined): ThemePreference => {
  switch (value) {
    case "light":
    case "dark":
    case "system":
      return value;
    default:
      return DEFAULT_THEME_PREFERENCE;
  }
};

export const resolveThemePreference = (preference: ThemePreference, systemPrefersDark: boolean): ResolvedTheme => {
  switch (preference) {
    case "light":
      return "light";
    case "dark":
      return "dark";
    case "system":
      return systemPrefersDark ? "dark" : "light";
  }
};

const readStoredThemePreference = (): ThemePreference => {
  try {
    return parseThemePreference(window.localStorage.getItem(SIMULATOR_THEME_STORAGE_KEY));
  } catch {
    return DEFAULT_THEME_PREFERENCE;
  }
};

const readSystemTheme = (): ResolvedTheme => {
  try {
    return resolveThemePreference("system", window.matchMedia(SIMULATOR_THEME_MEDIA_QUERY).matches);
  } catch {
    return "light";
  }
};

export const getInitialThemeState = (): { preference: ThemePreference; resolved: ResolvedTheme } => {
  const preference = readStoredThemePreference();
  return {
    preference,
    resolved: preference === "system" ? readSystemTheme() : preference,
  };
};

export const applyResolvedTheme = (theme: ResolvedTheme): void => {
  document.documentElement.dataset.theme = theme;
  document.documentElement.style.colorScheme = theme;
};

export const saveThemePreference = (preference: ThemePreference): void => {
  try {
    window.localStorage.setItem(SIMULATOR_THEME_STORAGE_KEY, preference);
  } catch {
    return;
  }
};

export function useSimulatorTheme() {
  const [state, setState] = useState(getInitialThemeState);

  useEffect(() => {
    applyResolvedTheme(state.resolved);
  }, [state.resolved]);

  useEffect(() => {
    if (state.preference !== "system") return undefined;

    const media = window.matchMedia(SIMULATOR_THEME_MEDIA_QUERY);
    const handleChange = (event: MediaQueryListEvent) => {
      setState((current) => current.preference === "system"
        ? { preference: "system", resolved: event.matches ? "dark" : "light" }
        : current);
    };
    media.addEventListener("change", handleChange);
    return () => media.removeEventListener("change", handleChange);
  }, [state.preference]);

  const setPreference = (preference: ThemePreference) => {
    saveThemePreference(preference);
    const resolved = preference === "system"
      ? readSystemTheme()
      : preference;
    setState({ preference, resolved });
  };

  return { ...state, setPreference };
}
