import path from 'path';

import createCommandManager from './Core/CommandCore/createCommandManager.js';
import "./bootstrap/app.js";

(async function () {

    const commandsDir = path.join(CONSTANTS.DIR, 'app', 'Commands');

    const commander = await createCommandManager(commandsDir);

    commander.execute();

})();