/* eslint-env serviceworker */
import { pass, assert } from "./assertions.js";
import * as configStore from 'fastly:config-store'
import { routes } from "./routes.js";

const { ConfigStore } = configStore;

routes.set("/config-store", () => {
  let config = new ConfigStore("testconfig");
  let twitterValue = config.get("twitter");
  let error = assert(twitterValue, "https://twitter.com/fastly", `config.get("twitter") === "https://twitter.com/fastly"`);
  if (error) { return error;}
  return pass()
});
