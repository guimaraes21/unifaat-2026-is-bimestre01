import express from 'express';

import web from './web.js';
import fileUpload from 'express-fileupload';
import cookieParser from 'cookie-parser';

export default (function () {

    const app = express();

    /** Usado para servir json */
    app.use(express.json());

    /** Usado para pode utilizar cookies */
    app.use(cookieParser());

    app.use(express.urlencoded({ extended: true }));

    app.use(fileUpload());

    //// Views
    app.use('/', web);

    /** Se nenhuma rota for encontrada, 404 neles! */
    app.use((request, response) => {
        response.status(CONSTANTS.HTTP.NOT_FOUND).json({ error: "Not found" });
    });

    return app;

})();
