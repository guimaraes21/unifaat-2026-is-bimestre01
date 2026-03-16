import { Router } from 'express';

import JsonHelloController from '../app/Http/Controllers/JsonHelloController.js';

export default (function () {

    const router = Router();

    router.get('/', JsonHelloController);


    return router;

})();
