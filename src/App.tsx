import { SimulatorDevicePane } from "./components/SimulatorDevicePane";
import "./panda-ide.css";
import "./App.css";
import { useSimulatorTheme } from "./theme";

function App() {
  const theme = useSimulatorTheme();
  return <SimulatorDevicePane themePreference={theme.preference} onThemePreferenceChange={theme.setPreference} />;
}

export default App;
