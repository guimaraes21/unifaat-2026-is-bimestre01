

export default async function JsonHelloController(request, response) {

    const HTTP_STATUS = CONSTANTS.HTTP;

    return response.status(HTTP_STATUS.SUCCESS).json({
        "message": "Hello, World!"
    });
};
